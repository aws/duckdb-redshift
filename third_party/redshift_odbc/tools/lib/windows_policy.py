"""The Windows loading policy of verification, applied to each file of the set the MSI holds.

    python3 -I windows_policy.py policy <policy-dir> <interface-header>
    python3 -I windows_policy.py check <set-dir> <driver> <policy-dir> <interface-header>
    python3 -I windows_policy.py rows <set-dir> <driver> <policy-dir> <kind>

<set-dir> holds the set's files as lib/msi.py check writes them, the driver and each library, and
<driver> is the driver's name there. <policy-dir> is policy/: its windows/ files, its
host-prerequisites.txt windows rows and windows/unshipped-names.txt. DLL names compare ignoring
case, as Windows compares them; exported names compare exactly, as GetProcAddress does. Every file
is read by lib/pe.py, which refuses the forms it does not read; nothing in it is executed.

policy checks, before verification downloads anything, that the three files below are well formed
and that expected-exports.txt holds every entry point of the interface header's
REDSHIFT_ODBC_ENTRY_POINTS, as SQL<name>; it fails at policy or entry-points.

check applies these rules to each file, printing every problem, one per line, as "<rule>: <file>:
<reason>", and exits 1 if there is one:
  image          a PE32+ image for x86-64, without a .NET runtime header;
  imports        the DLL names of its import descriptors, its delay imports and its forwarders'
                 targets (".dll" added when a target has none) equal its rows of
                 allowed-imports.txt (<file> <dll>), both ways: each a set member or a system DLL
                 a reviewer named, since the delay helper and the loader load those by name too;
  prerequisites  each VC++ run-time DLL it imports (msvcr, msvcp, vcruntime, concrt, vccorlib,
                 vcomp, vcamp or mfc, then a version number) that is not a set member is a windows
                 row of host-prerequisites.txt;
  delay-imports  no delay import names a set member;
  bound-imports  it has no bound-import directory;
  forwarders     no forwarder names a set member;
  exports        the driver's exported names, and "@<ordinal>" for an export with no name, equal
                 expected-exports.txt (one per row), both ways;
  load-flags     its load configuration's DependentLoadFlags is 0, or holds
                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR and no bit but it and
                 LOAD_LIBRARY_SEARCH_SYSTEM32, since the flags replace the loader's for its imports;
  runtime-names  the names it holds for run-time loads equal its rows of runtime-names.txt
                 (<file> <string>, the string the rest of the row), both ways: every whole
                 NUL-terminated ASCII or UTF-16LE string (lib/pe.py whole_strings) that ends in .dll
                 or equals a base name of a set file, of a DLL in allowed-imports.txt or
                 runtime-names.txt, or of an unshipped DLL, outside the export name, the version
                 resource, the certificate table, the PDB path and the import and delay-import
                 names and hint/name entries. So a bare "dbghelp" is found once any row names
                 dbghelp.dll, "sslmode" is no "ssl" hit, and an imported SSL_CTX_new is none either.
A row naming a file the set does not hold is a problem too. A form lib/pe.py does not read fails
under the rule that reads it: image (the headers, the debug directory), imports (an import or
delay-import table, by virtual address too, or an import's name outside its section), exports (the
export directory, its tables, names and forwarders) or version (the version resource). On success
it prints one line per file, with its counts and its version resource's OriginalFilename.

The policy files are read as driver_info.py reads them: a token that starts with "#" starts a
comment, to the end of its line.

rows prints the body of one policy file, <kind> being allowed-imports (delay imports and
forwarders' targets too), expected-exports or runtime-names, which is how the files are written on a
driver update and then reviewed: one row per file and name, runtime-names with the base names grown
from the strings found until no new one is, and expected-exports the driver's exports. On failure
each command prints "<rule>: <reason>" and exits 1. Standard library only, Python 3.6 and later.
"""

import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DLL_LOAD_DIR, SYSTEM32 = 0x100, 0x800
VC_RUNTIME = re.compile(r"(msvcr|msvcp|vcruntime|concrt|vccorlib|vcomp|vcamp|mfc)[0-9]+[a-z0-9_]*"
                        r"\.dll\Z", re.I)
FILE_NAME = re.compile(r"[A-Za-z0-9_.+-]+\Z")
COMMENT = re.compile(r"(?:\A|\s)#")
UNSHIPPED_NAME = re.compile(r"[a-z0-9_]{1,64}\Z")
KINDS = ("allowed-imports", "expected-exports", "runtime-names")


def sibling(name):
    """A module of this directory, by path, since python3 -I puts no directory on sys.path. No
    bytecode is written next to it, so the tools never change the repository."""
    sys.dont_write_bytecode = True
    path = os.path.join(HERE, name + ".py")
    spec = importlib.util.spec_from_file_location("redshift_odbc_" + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pe = sibling("pe")
macho = sibling("macho")  # entry_points reads the interface header's list, for macOS too
lock = sibling("lock")
Failure = pe.Failure


def base(name):
    return name[:-4] if name.lower().endswith(".dll") else name


def lines(path):
    """(text, line number) of each row: a token that starts with "#" starts a comment, as in
    driver_info.policy_rows, and a line with nothing before its comment is no row."""
    try:
        with open(path, encoding="ascii") as f:
            texts = [(COMMENT.split(l, 1)[0].strip(), n) for n, l in enumerate(f, 1)]
    except (OSError, UnicodeDecodeError) as e:
        raise Failure("policy: cannot read %s: %s" % (path, e))
    return [(t, n) for t, n in texts if t]


def search_bases(members, named, unshipped):
    """The lower-case base names the run-time name search looks for; ".dll" alone names none."""
    return {base(n).lower() for n in list(members) + list(named) if base(n).strip()} | unshipped


def unshipped_names(policy_dir):
    """Each row a lower-case base name, as the rename's tools require (rename_apply.policy_names):
    a row with .dll would otherwise match no string the run-time name search reads."""
    names = set()
    for text, n in lines(os.path.join(policy_dir, "windows", "unshipped-names.txt")):
        if not UNSHIPPED_NAME.match(text):
            raise Failure("policy: unshipped-names.txt:%d: %r is not a lower-case base name" % (n, text))
        names.add(text)
    return names


class Policy(object):
    """policy/windows/'s files and the windows host prerequisites: imports and runtime are {file:
    {name: where listed}}, exports {name: where listed}, every name but an export's lower case."""

    def __init__(self, policy_dir):
        windows = os.path.join(policy_dir, "windows")
        self.imports = self.per_file(os.path.join(windows, "allowed-imports.txt"), "dll")
        self.runtime = self.per_file(os.path.join(windows, "runtime-names.txt"), "string")
        self.exports = {}
        for text, n in lines(os.path.join(windows, "expected-exports.txt")):
            if len(text.split()) != 1 or text in self.exports:
                raise Failure("policy: expected-exports.txt:%d: a row is one exported name, listed "
                              "once" % n)
            self.exports[text] = "expected-exports.txt:%d" % n
        self.prerequisites = set()
        for text, n in lines(os.path.join(policy_dir, "host-prerequisites.txt")):
            row = text.split()
            if row[0] == "windows" and len(row) != 2:
                raise Failure("policy: host-prerequisites.txt:%d: a windows row is windows <dll>"
                              % n)
            if row[0] == "windows":
                self.prerequisites.add(row[1].lower())
        self.unshipped = unshipped_names(policy_dir)

    @staticmethod
    def per_file(path, what):
        """{file: {name: "<policy file>:<line>"}} of a file of <file> <name> rows."""
        table, kind = {}, os.path.basename(path)
        for text, n in lines(path):
            parts = text.split(None, 1)
            if len(parts) != 2 or not FILE_NAME.match(parts[0]) or (
                    what == "dll" and not FILE_NAME.match(parts[1])):
                raise Failure("policy: %s:%d: a row is <file> <%s>" % (kind, n, what))
            rows = table.setdefault(parts[0].lower(), {})
            if parts[1].lower() in rows:
                raise Failure("policy: %s:%d: %s is listed twice for %s"
                              % (kind, n, parts[1], parts[0]))
            rows[parts[1].lower()] = "%s:%d" % (kind, n)
        return table

    def entry_points(self, header):
        try:
            missing = sorted(macho.entry_points(header) - set(self.exports))
        except macho.Failure as e:
            raise Failure(str(e)) from e
        if missing:
            raise Failure("entry-points: expected-exports.txt lacks the interface's entry points %s"
                          % " ".join(missing))

    def bases(self, members):
        named = [n for t in (self.imports, self.runtime) for rows in t.values() for n in rows]
        return search_bases(members, named, self.unshipped)


def naming(name, failure):
    """A reader's "<rule>: <reason>" failure as "<rule>: <file>: <reason>"."""
    rule, _, reason = str(failure).partition(": ")
    return Failure("%s: %s: %s" % (rule, name, reason))


def read_set(set_dir, driver):
    """{name: Image} of the set's files, which must include the driver."""
    images = {}
    try:
        names = sorted(os.listdir(set_dir))
    except OSError as e:
        raise Failure("set: cannot read %s: %s" % (set_dir, e.strerror)) from e
    for name in names:
        path = os.path.join(set_dir, name)
        if lock.name_problem(name) or os.path.islink(path) or not os.path.isfile(path):
            raise Failure("set: %s"
                          % (lock.name_problem(name) or "%s is not a regular file" % path))
        try:
            with open(path, "rb") as f:
                if os.fstat(f.fileno()).st_size > pe.MAX_FILE:
                    raise Failure("image: %s: the file is larger than 1 GiB" % name)
                data = f.read(pe.MAX_FILE)
        except OSError as e:
            raise Failure("set: cannot read %s: %s" % (path, e.strerror)) from e
        try:
            images[name] = pe.Image(data)
        except Failure as e:
            raise naming(name, e) from e
    if driver not in images:
        raise Failure("set: %s holds no %s" % (set_dir, driver))
    return images


def forwarded_dll(forwarder):
    """The DLL a forwarder "<dll>.<name>" names, as the loader loads it: .dll added when missing."""
    target = forwarder.decode("ascii", "replace").rsplit(".", 1)[0]
    return target if target.lower().endswith(".dll") else target + ".dll"


def imported(image):
    """{lower-case name: name} of the DLLs the image imports, by import descriptor, delay import or
    forwarder: the delay helper loads its DLL by name when a function is first called, and the
    loader a forwarder's, with .dll added, when its export is resolved."""
    names = [i.name for i in pe.imports(image)] + [d for _, d in pe.delay_imports(image)]
    names = [n.decode("ascii", "replace") for n in names]
    names += [forwarded_dll(f) for f in pe.exports(image)[1]]
    return {n.lower(): n for n in names}


def runtime_names(image, bases):
    """{lower-case string: string} of each run-time name the image holds."""
    skip = pe.names_allowed(image)
    skip += [(i.name_at, i.name_at + len(i.name) + 1) for i in pe.imports(image)]
    skip += [(at, at + len(name) + 1) for at, name in pe.delay_imports(image)]
    found = {}
    for at, _, _, text in pe.whole_strings(image.data):
        key = text.strip().lower()  # a row can neither start nor end with a space
        if (key.endswith(".dll") or key in bases) and not any(a <= at < b for a, b in skip):
            found.setdefault(key, text.strip())
    return found


def compare(rule, name, got, listed, source, verbs):
    """The problems of {name: as shown} against {name: where listed}, both ways; verbs are the
    image's, as in "it imports", and their base form."""
    return (["%s: %s: it %s %s, which %s does not list" % (rule, name, verbs[0], got[k], source)
             for k in sorted(set(got) - set(listed))] +
            ["%s: %s: %s lists %s, which it does not %s" % (rule, name, listed[k], k, verbs[1])
             for k in sorted(set(listed) - set(got))])


def check_file(name, image, members, bases, policy, is_driver):
    """(problems, the file's summary line) of one set file."""
    problems = []
    if image.directory(pe.CLR)[0]:
        problems.append("image: %s: it has a .NET runtime header, a form this reader does not read"
                        % name)
    got = imported(image)
    problems += compare("imports", name, got, policy.imports.get(name.lower(), {}),
                        "allowed-imports.txt", ("imports", "import"))
    problems += ["prerequisites: %s: it imports the VC++ run-time DLL %s, which "
                 "host-prerequisites.txt does not list for windows" % (name, got[d])
                 for d in sorted(got)
                 if VC_RUNTIME.match(d) and d not in members and d not in policy.prerequisites]
    member_bases = {base(m) for m in members}  # LoadLibrary adds .dll to a name without one
    delayed = [d.decode("ascii", "replace") for _, d in pe.delay_imports(image)]
    problems += ["delay-imports: %s: it delay-loads the set member %s" % (name, d)
                 for d in delayed if base(d).lower() in member_bases]
    if image.directory(pe.BOUND_IMPORT)[0]:
        problems.append("bound-imports: %s: it has bound imports" % name)
    for forwarder in (f.decode("ascii", "replace") for f in pe.exports(image)[1]):
        if base(forwarder.rsplit(".", 1)[0]).lower() in member_bases:
            problems.append("forwarders: %s: it forwards %s to a set member" % (name, forwarder))
    if is_driver:
        exported = {e: e for e in pe.export_names(image)}
        problems += compare("exports", name, exported, policy.exports, "expected-exports.txt",
                            ("exports", "export"))
    flags = pe.dependent_load_flags(image)
    if flags and (not flags & DLL_LOAD_DIR or flags & ~(DLL_LOAD_DIR | SYSTEM32)):
        problems.append("load-flags: %s: its DependentLoadFlags 0x%x is not "
                        "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR with at most "
                        "LOAD_LIBRARY_SEARCH_SYSTEM32" % (name, flags))
    found = runtime_names(image, bases)
    problems += compare("runtime-names", name, found, policy.runtime.get(name.lower(), {}),
                        "runtime-names.txt", ("holds the run-time name", "hold"))
    original = [v for k, v in pe.version_strings(image) if k == "OriginalFilename"]
    # Each kind counted by DLL, as allowed-imports.txt lists them.
    descriptors = {i.name.lower() for i in pe.imports(image)}
    forwarded = {forwarded_dll(f).lower() for f in pe.exports(image)[1]}
    return problems, ("%s: %d imports, %d delay imports, %d forwarded DLLs, %d run-time names, "
                      "OriginalFilename %s" % (name, len(descriptors), len({d.lower() for d in delayed}),
                                               len(forwarded), len(found), ",".join(original) or "none"))


def check(set_dir, driver, policy_dir, header):
    """(problems, a summary line per file) of the set against the policy."""
    policy = Policy(policy_dir)
    policy.entry_points(header)
    images = read_set(set_dir, driver)
    members = {n.lower() for n in images}
    problems, summary = [], []
    for kind, table in (("allowed-imports.txt", policy.imports),
                        ("runtime-names.txt", policy.runtime)):
        problems += ["policy: %s names %s, which is not a file of the set" % (kind, n)
                     for n in sorted(set(table) - members)]
    bases = policy.bases(members)
    for name in sorted(images):
        try:
            found, line = check_file(name, images[name], members, bases, policy, name == driver)
        except Failure as e:
            raise naming(name, e) from e
        problems += found
        summary.append(line)
    return problems, summary


def rows(set_dir, driver, policy_dir, kind):
    """The rows of policy/windows/<kind>.txt that the set meets."""
    images = read_set(set_dir, driver)
    if kind == "expected-exports":
        return sorted(pe.export_names(images[driver]))
    imports = {n: sorted(imported(images[n]).values(), key=str.lower) for n in sorted(images)}
    if kind == "allowed-imports":
        return ["%s  %s" % (n, d) for n in imports for d in imports[n]]
    named, found = [d for n in imports for d in imports[n]], {}
    unshipped = unshipped_names(policy_dir)
    while True:  # a string found that ends in .dll adds its base name, which can find more strings
        bases = search_bases(images, named + [s for n in found for s in found[n]], unshipped)
        now = {n: runtime_names(images[n], bases) for n in sorted(images)}
        if now == found:
            return ["%s  %s" % (n, s) for n in now for _, s in sorted(now[n].items())]
        found = now


def main(argv):
    try:
        if len(argv) == 4 and argv[1] == "policy":
            Policy(argv[2]).entry_points(argv[3])
            print("the Windows policy files are well formed, and expected-exports.txt holds every "
                  "entry point")
        elif len(argv) == 6 and argv[1] == "check":
            problems, summary = check(*argv[2:])
            if problems:
                sys.stderr.write("".join(p + "\n" for p in problems))
                return 1
            print("the set's %d files meet the Windows loading policy" % len(summary))
            print("\n".join(summary))
        elif len(argv) == 6 and argv[1] == "rows" and argv[5] in KINDS:
            print("\n".join(rows(*argv[2:])))
        else:
            sys.stderr.write("usage: windows_policy.py policy <policy-dir> <interface-header> | "
                             "check <set-dir> <driver> <policy-dir> <interface-header> | rows "
                             "<set-dir> <driver> <policy-dir> <%s>\n" % "|".join(KINDS))
            return 2
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
