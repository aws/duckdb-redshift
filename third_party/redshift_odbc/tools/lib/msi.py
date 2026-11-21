"""Reads an MSI's tables and takes its files out of its cabinet, from Linux.

    python3 -I msi.py policy <policy-dir>
    python3 -I msi.py record <msi>
    python3 -I msi.py check <msi> <policy-dir> <version> <msi-url> <msi-sha256> <out-dir>
    python3 -I msi.py extract <msi> <out-dir> <extract-path>=<name>...
    python3 -I msi.py compare <msi> <admin-image-dir>

The MSI is read through lib/cfb.py's strict compound-file reader: the string pool, the File,
Component, Directory and Media tables, Property, CustomAction and the two administrative
sequences, and the embedded cabinet, through lib/cab.py. A file's extract path is where `msiexec
/a` puts it: the Directory table's source names, long form, from TARGETDIR down, then the file's
long name, with a space written %20 as the lock writes it.

policy checks that policy/windows/product.txt and msi-files.txt are well formed, so verification
refuses a broken policy before it downloads anything. record prints what writing msi-files.txt
needs of an MSI opened read-only: the product's name and version, every action of the
administrative sequences and whether it is a custom action, and every file as a row of
policy/windows/msi-files.txt (marked leave, to change by hand to driver or library for the set's
files) with its extract path, size and SHA-256.

check applies verification's files rule: Property names the product of policy/windows/product.txt
and <version>; no administrative sequence runs a custom action; the File table holds exactly the
files msi-files.txt lists, by key, long name and directory; each comes from an embedded cabinet,
none is named exactly like a notice, a LICENSE.txt, NOTICE.txt or THIRD_PARTY_LICENSES.txt has
exactly the bytes of the checked-in notice beside <policy-dir> (the release tag's copy, which ships),
and each driver or library file decodes, within the bounds, to its recorded size and is a PE32+
image for x86-64. It writes those files into <out-dir>, which must
be empty, and prints their lock rows, the driver's first, each checked by the lock's row rules.

extract writes the file at each <extract-path> to <out-dir>/<name>, for fetch-driver.sh, which
checks each against the lock. compare checks, on Windows after `msiexec /a /qn`, that the
administrative image holds every file of the MSI at its extract path with the bytes this reader
decodes, and prints each file's SHA-256. On failure each prints one line, "files: <reason>" (or
cfb's or cab's), and exits 1. Standard library only, Python 3.6 and later.
"""

import hashlib
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_FILE, MAX_TOTAL = 64 << 20, 128 << 20
MIME = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._"
NOTICES = ("license", "notice", "third_party_licenses")
# The MSI's own copies of the notices, which 2.3.1.0's MSI carries as <name>.txt: left in the MSI, as the
# release tag's copies ship, and each must have the tag's bytes, which the checked-in copy beside policy/ has.
TXT_NOTICES = dict((n + ".txt", n.upper()) for n in NOTICES)
ADMIN = ("AdminExecuteSequence", "AdminUISequence")
NONCOMPRESSED = 0x2000  # msidbFileAttributesNoncompressed: a loose file beside the MSI
KINDS = ("driver", "library", "leave")


def load(name):
    """A module from this directory, by its file, so nothing else on sys.path is searched."""
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("redshift_odbc_" + name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cfb, cab, lock, pe = load("cfb"), load("cab"), load("lock"), load("pe")


class Failure(Exception):
    pass


def stream_name(name, table):
    """An MSI stream's name as the compound file stores it: pairs of identifier characters packed
    into one character each, and a table's behind the 0x4840 marker."""
    out, i = ["\u4840"] if table else [], 0
    while i < len(name):
        a = MIME.find(name[i])
        b = MIME.find(name[i + 1]) if a >= 0 and i + 1 < len(name) else -1
        if b >= 0:
            out.append(chr(0x3800 + a + (b << 6)))
            i += 2
        else:
            out.append(chr(0x4800 + a) if a >= 0 else name[i])
            i += 1
    return "".join(out)


class Database(object):
    def __init__(self, cf):
        self.cf = cf
        pool, data = self.stream("_StringPool"), self.stream("_StringData")
        if pool is None or data is None or len(pool) % 4 or len(pool) < 4:
            raise Failure("files: the MSI has no string pool this tool reads")
        words = struct.unpack("<%dH" % (len(pool) // 2), pool)
        codepage = words[0] | (words[1] & 0x7FFF) << 16
        self.width = 3 if words[1] & 0x8000 else 2
        encoding = {0: "cp1252", 1252: "cp1252", 65001: "utf-8"}.get(codepage)
        if encoding is None:
            raise Failure("files: the string pool's code page %d is not one this tool reads" % codepage)
        self.strings, offset = [None], 0
        for i in range(2, len(words), 2):
            length, refs = words[i], words[i + 1]
            if length == 0 and refs:
                raise Failure("files: the string pool holds a string over 64 KiB")
            try:
                self.strings.append(data[offset:offset + length].decode(encoding))
            except UnicodeDecodeError:
                raise Failure("files: string %d is not %s" % (len(self.strings), encoding))
            offset += length
        if offset != len(data):
            raise Failure("files: the string pool and its data disagree on their length")
        tables = self.stream("_Tables") or b""
        self.tables = [self.string(r) for r in self.refs(tables, len(tables) // self.width)]
        columns = self.stream("_Columns") or b""
        count = len(columns) // (2 * self.width + 4)
        if len(columns) % (2 * self.width + 4):
            raise Failure("files: the _Columns table's size is not a whole number of rows")
        table_refs = self.refs(columns, count)
        numbers = struct.unpack_from("<%dH" % count, columns, count * self.width)
        names = self.refs(columns[count * (self.width + 2):], count)
        types = struct.unpack_from("<%dH" % count, columns, count * (2 * self.width + 2))
        self.columns = {}
        for t, n, name, kind in zip(table_refs, numbers, names, types):
            self.columns.setdefault(self.string(t), []).append((n - 0x8000, self.string(name), kind - 0x8000))

    def stream(self, name, table=True):
        return self.cf.stream(self.cf.root, stream_name(name, table))

    def refs(self, data, count):
        if self.width == 2:
            return struct.unpack_from("<%dH" % count, data)
        return [data[i] | data[i + 1] << 8 | data[i + 2] << 16 for i in range(0, 3 * count, 3)]

    def string(self, ref):
        if ref >= len(self.strings):
            raise Failure("files: a table names string %d, past the pool's end" % ref)
        return self.strings[ref]

    def rows(self, table, required=True):
        """Every row of <table> as a dict of its columns, in stored order."""
        if table not in self.tables:
            if required:
                raise Failure("files: the MSI has no %s table" % table)
            return []
        columns = sorted(self.columns.get(table, []))
        if not columns:
            raise Failure("files: the MSI's _Columns table has no column of %s" % table)
        if [n for n, _, _ in columns] != list(range(1, len(columns) + 1)):
            raise Failure("files: the %s table's columns are not numbered 1 to %d" % (table, len(columns)))
        # Stored column by column: a string as its pool reference, a binary column's stream as 2
        # bytes, an integer in 2 or 4 bytes biased by half its range, with 0 for null.
        binary = [kind & ~0x1000 == 0x0900 for _, _, kind in columns]
        widths = [2 if b else self.width if kind & 0x0800 else 2 if kind & 0xFF <= 2 else 4
                  for b, (_, _, kind) in zip(binary, columns)]
        data = self.stream(table) or b""
        if len(data) % sum(widths):
            raise Failure("files: the %s table's size is not a whole number of rows" % table)
        count, at = len(data) // sum(widths), 0
        out = [dict() for _ in range(count)]
        for (_, name, kind), width, is_binary in zip(columns, widths, binary):
            for i in range(count):
                raw = int.from_bytes(data[at + i * width:at + (i + 1) * width], "little")
                if is_binary:
                    out[i][name] = None
                elif kind & 0x0800:
                    out[i][name] = self.string(raw)
                else:
                    out[i][name] = raw - (1 << (8 * width - 1)) if raw else None
            at += count * width
        return out


def long_name(field):
    """A Filename or DefaultDir name: the long form after |, when there is one."""
    return field.split("|", 1)[-1]


def directory_paths(db):
    """{Directory key: [segments]}, from TARGETDIR down, with each directory's source name."""
    table = dict((r["Directory"], r) for r in db.rows("Directory"))
    paths = {}

    def path(key, depth):
        if key in paths:
            return paths[key]
        if depth > len(table) or key not in table:
            raise Failure("files: the Directory table's parents of %s loop or are missing" % key)
        row = table[key]
        parent = row["Directory_Parent"]
        if parent in (None, key):
            if key != "TARGETDIR":
                raise Failure("files: directory %s has no parent and is not TARGETDIR" % key)
            paths[key] = []
            return paths[key]
        target, _, source = row["DefaultDir"].partition(":")
        name = long_name(source or target)
        paths[key] = path(parent, depth + 1) + ([] if name == "." else [name])
        return paths[key]

    for key in table:
        if table[key]["Directory_Parent"] not in (None, key) or key == "TARGETDIR":
            path(key, 0)
    return paths


def read(msi_path):
    """The MSI's database, and every file as (key, long name, directory key, extract path, size,
    sequence, attributes)."""
    cf = cfb.load(msi_path)
    db = Database(cf)
    components = dict((r["Component"], r["Directory_"]) for r in db.rows("Component"))
    paths = directory_paths(db)
    files = []
    for r in db.rows("File"):
        directory = components.get(r["Component_"])
        if directory not in paths:
            raise Failure("files: file %s is in no directory under TARGETDIR" % r["File"])
        name = long_name(r["FileName"])
        path = "./" + "/".join(paths[directory] + [name]).replace(" ", "%20")
        files.append((r["File"], name, directory, path, r["FileSize"], r["Sequence"], r["Attributes"] or 0))
    return db, files


def contents(db, files):
    """{File key: bytes} for every file, from the embedded cabinets the Media table names."""
    media = sorted((r["LastSequence"], r["Cabinet"]) for r in db.rows("Media"))
    out = {}
    for key, _, _, _, _, sequence, attributes in files:
        if attributes & NONCOMPRESSED:
            raise Failure("files: %s is stored outside the MSI's cabinets" % key)
        cabinet = next((c for last, c in media if sequence <= last), None)
        if not cabinet or not cabinet.startswith("#"):
            raise Failure("files: %s is in no cabinet embedded in the MSI" % key)
    # MAX_TOTAL bounds every cabinet together, so each is given only what the earlier ones left.
    budget = MAX_TOTAL
    for _, cabinet in media:
        if not cabinet:
            continue
        data = db.stream(cabinet[1:], table=False)
        if data is None:
            raise Failure("files: the MSI has no stream for its cabinet %s" % cabinet)
        # A later cabinet's bound is only what the earlier ones left, so its refusal says so.
        left = "" if budget == MAX_TOTAL else ", what the earlier cabinets left of the %d bytes" % MAX_TOTAL
        try:
            decoded = cab.files(data, MAX_FILE, budget, left)
        except cab.Failure as e:
            raise cab.Failure("%s (in cabinet %s)" % (e, cabinet))
        for name, body in decoded.items():
            if name in out:
                raise Failure("files: two cabinets hold %s" % name)
            out[name] = body
            budget -= len(body)
    keys = set(f[0] for f in files)
    extra = sorted(set(out) - keys)
    if extra:
        raise Failure("files: the cabinet holds %s, which the File table does not name" % extra[0])
    for key, _, _, _, size, _, _ in files:
        if key not in out or len(out[key]) != size:
            raise Failure("files: %s decodes to %s bytes, and the File table says %d"
                          % (key, len(out[key]) if key in out else "no", size))
    return out


def product(db):
    properties = dict((r["Property"], r["Value"]) for r in db.rows("Property"))
    return properties.get("ProductName"), properties.get("ProductVersion")


def custom_actions(db):
    """[(sequence table, action)] for every administrative action that is a custom action."""
    custom = set(r["Action"] for r in db.rows("CustomAction", required=False))
    return [(t, r["Action"]) for t in ADMIN for r in db.rows(t, required=False) if r["Action"] in custom]


def policy(policy_dir):
    """The product name, and [(kind, key, long name, directory)] from msi-files.txt."""
    rows = []
    for name in ("product.txt", "msi-files.txt"):
        path = os.path.join(policy_dir, "windows", name)
        try:
            with open(path, encoding="ascii") as f:
                lines = [l.split("#", 1)[0].split() for l in f]
        except (OSError, UnicodeDecodeError) as e:
            raise Failure("policy: policy/windows/%s cannot be read: %s" % (name, e))
        rows.append([l for l in lines if l])
    (names, files) = rows
    if len(names) != 1:
        raise Failure("policy: policy/windows/product.txt holds one line, the ProductName")
    out = []
    for tokens in files:
        if len(tokens) != 4 or tokens[0] not in KINDS:
            raise Failure("policy: a row of policy/windows/msi-files.txt is <driver|library|leave> "
                          "<File key> <long name> <Directory key>, a space written %20")
        out.append((tokens[0], tokens[1], tokens[2].replace("%20", " "), tokens[3]))
    if [k for k, _, _, _ in out].count("driver") != 1:
        raise Failure("policy: policy/windows/msi-files.txt must mark exactly one file driver")
    return " ".join(names[0]).replace("%20", " "), out


def check(msi_path, policy_dir, version, url, msi_sha, out_dir):
    want_product, listed = policy(policy_dir)
    db, files = read(msi_path)
    got = product(db)
    if got != (want_product, version):
        raise Failure("files: the signed MSI is %r version %r, not %r version %r" % (got + (want_product, version)))
    actions = custom_actions(db)
    if actions:
        raise Failure("files: %s runs the custom action %s, which msiexec /a would run" % actions[0])
    found = set((key, name, directory) for key, name, directory, _, _, _, _ in files)
    wanted = set((key, name, directory) for _, key, name, directory in listed)
    for what, items in (("holds %s, which policy/windows/msi-files.txt does not list", found - wanted),
                        ("lacks %s, which policy/windows/msi-files.txt lists", wanted - found)):
        if items:
            raise Failure("files: the MSI " + what % " ".join(sorted(items)[0]))
    for _, name, _, _, _, _, _ in files:
        if name.lower() in NOTICES:
            raise Failure("files: the MSI holds %s, named like a notice but not the MSI's own <notice>.txt copy, "
                          "which alone may be there, with the release tag's bytes" % name)
    bodies = contents(db, files)
    odbc_dir = os.path.dirname(os.path.abspath(policy_dir))
    for key, name, _, _, _, _, _ in files:
        notice = TXT_NOTICES.get(name.lower())
        if notice is None:
            continue
        try:
            with open(os.path.join(odbc_dir, notice), "rb") as f:
                want = hashlib.sha256(f.read()).hexdigest()
        except OSError as e:
            raise Failure("files: the checked-in %s cannot be read: %s" % (notice, e.strerror))
        got = hashlib.sha256(bodies[key]).hexdigest()
        if got != want:
            raise Failure("files: the MSI's %s has SHA-256 %s; the checked-in %s, the release tag's copy, has %s"
                          % (name, got, notice, want))
    if os.listdir(out_dir):
        raise Failure("files: %s is not empty" % out_dir)
    by_key = dict((f[0], f) for f in files)
    rows = []
    for kind, key, name, _ in sorted(listed, key=lambda r: r[0] != "driver"):
        if kind == "leave":
            continue
        body = bodies[key]
        try:
            pe.Image(body)
        except pe.Failure as e:
            raise Failure("files: %s is not a PE32+ image for x86-64: %s" % (name, e))
        sha = hashlib.sha256(body).hexdigest()
        tokens = ["windows_amd64", kind, name, sha, sha, url, msi_sha, by_key[key][3]]
        _, problems = lock.check_windows_row(tokens, version)
        if problems:
            raise Failure("files: the lock would refuse %s's row: %s" % (name, problems[0]))
        with open(os.path.join(out_dir, name), "xb") as f:
            f.write(body)
        rows.append("  ".join(tokens))
    return rows


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "policy":
            policy(argv[2])
        elif len(argv) == 3 and argv[1] == "record":
            db, files = read(argv[2])
            bodies = contents(db, files)
            name, version = product(db)
            print("ProductName %s\nProductVersion %s" % (name, version))
            custom = set(r["Action"] for r in db.rows("CustomAction", required=False))
            for t in ADMIN:
                for r in sorted(db.rows(t, required=False), key=lambda r: r["Sequence"] or 0):
                    print("%s %s %s%s" % (t, r["Sequence"], r["Action"], " CUSTOM ACTION" if r["Action"] in custom else ""))
            for key, long, directory, path, size, _, _ in files:
                print("leave  %s  %s  %s  # %s %d %s" % (key, long.replace(" ", "%20"), directory, path, size,
                                                       hashlib.sha256(bodies[key]).hexdigest()))
        elif len(argv) == 8 and argv[1] == "check":
            for row in check(*argv[2:]):
                print(row)
        elif len(argv) >= 4 and argv[1] == "extract":
            db, files = read(argv[2])
            bodies, by_path = contents(db, files), dict((f[3], f[0]) for f in files)
            for pair in argv[4:]:
                path, _, name = pair.partition("=")
                if path not in by_path or lock.name_problem(name):
                    raise Failure("files: the MSI has no file at %s, or %r is not a file name" % (path, name))
                with open(os.path.join(argv[3], name), "xb") as f:
                    f.write(bodies[by_path[path]])
        elif len(argv) == 4 and argv[1] == "compare":
            db, files = read(argv[2])
            bodies, lines = contents(db, files), []
            root = os.path.abspath(argv[3])
            for key, _, _, path, _, _, _ in files:
                on_disk = os.path.abspath(os.path.join(root, *path[2:].replace("%20", " ").split("/")))
                # The path comes from the MSI's Directory and File tables. A ".." can leave the image
                # anywhere; on Windows a drive or a backslash can too, and another drive makes
                # commonpath raise.
                try:
                    inside = os.path.commonpath([root, on_disk]) == root
                except ValueError:
                    inside = False
                if not inside:
                    raise Failure("files: %s is outside the administrative image" % path)
                try:
                    with open(on_disk, "rb") as f:
                        disk = f.read(MAX_FILE + 1)
                except OSError as e:
                    raise Failure("files: the administrative image has no %s: %s" % (path, e.strerror))
                if disk != bodies[key]:
                    raise Failure("files: %s in the administrative image differs from the cabinet's" % path)
                lines.append("%s  %s" % (hashlib.sha256(disk).hexdigest(), path))
            print("\n".join(lines))
        else:
            sys.stderr.write("usage: msi.py policy <policy-dir> | record <msi> | check <msi> <policy-dir> <version> <msi-url> "
                             "<msi-sha256> <out-dir> | extract <msi> <out-dir> <extract-path>=<name>... | "
                             "compare <msi> <admin-image-dir>\n")
            return 2
    except (Failure, cfb.Failure, cab.Failure) as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except (struct.error, KeyError, TypeError, AttributeError) as e:  # AttributeError: a null name
        sys.stderr.write("files: the MSI's tables are not ones this tool reads: %r\n" % (e,))
        return 1
    except OSError as e:
        # An <out-dir> that is missing or not writable, named like every other failure.
        sys.stderr.write("files: %s: %s\n" % (e.filename, e.strerror))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
