"""Checks a driver lock and prints it in normalised form (README.md, "The lock").

    python3 -I lock.py <lock>

tools/check-lock.sh is the command; this is its implementation. fetch-driver.sh, and through it
driver_info.py, read a lock only through that command's output, so every name, hash, URL and path
they use has passed the rules below, and every assembler symbol stem is a C identifier.

On success it prints, and exits 0:

    format <1|2>
    version <version>
    status <development|release>
    driver <platform> <name> <sha256> <stem> <package-url> <package-sha256> <path-in-package>
    notice <name> <sha256> <stem> <source>

with one driver line per platform and one notice line per notice, each kind in lock order. Format
2 adds windows_amd64, whose set is several files taken from one MSI: its driver line ends with the
file's source-sha256, the SHA-256 of the file as the MSI holds it, and the lines

    library windows_amd64 <name> <sha256> <stem> <msi-url> <msi-sha256> <extract-path> <source-sha256>
    prerequisite windows_amd64 <name> <minimum file version>
    rename windows_amd64 <name> <embedded name>
    patch windows_amd64 <name> <offset> <original bytes> <new bytes>
    name-seed windows_amd64 <sha256>
    set-hash windows_amd64 <sha256>

follow the driver lines, each kind in lock order. A lock that renames the set records, once each,
the name seed its names derive from and the set hash of the renamed set; this reader checks their
form, rename_check.py each name against the seed and the seed against the one it recomputes from the
original set, and driver_info.py the set hash against the fetched set. An extract path writes a
space as %20, so every field stays one token. On failure it prints every problem found, one per line
with its line number, to stderr, prints nothing on stdout, and exits 1.

    python3 -I lock.py --version <version> | --path <path-in-package> | --name <file-name>

checks one part by the same rules a lock's part follows, for verification, which has no lock yet:
the version asked for, and the driver's path in the package and its file name from verification's
policy file (a driver file named like a notice is refused, as in a lock). It prints the problem and
exits 1, or exits 0. Standard library only; runs on Python 3.6 and later.
"""

import re
import sys

PLATFORMS = ("linux_amd64", "linux_arm64", "osx_amd64", "osx_arm64", "windows_amd64")
WINDOWS = "windows_amd64"
WINDOWS_KINDS = ("library", "prerequisite", "rename", "patch", "name-seed", "set-hash")
RENAME_HASHES = ("name-seed", "set-hash")
# The notices kept with the driver, so that every installed copy carries them, and the only files
# besides the driver a set may hold: a reviewed change to this list, never a lock edit alone, adds
# one.
REQUIRED_NOTICES = ("LICENSE", "NOTICE", "THIRD_PARTY_LICENSES")
FIRST_RELEASE = (2, 3, 0, 0)  # the first release that sends TLS SNI; below it, development only
# Names the build writes beside the set's files (fetch-driver.sh, driver_info.py), which a set file
# must not take.
RESERVED_NAMES = ("driver_info.h", "packages")
URL_PREFIX = "https://s3.amazonaws.com/redshift-downloads/drivers/odbc/"

VERSION = re.compile(r"(0|[1-9][0-9]{0,8})(\.(0|[1-9][0-9]{0,8})){3}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._-]{0,63}\Z")
URL_CHARS = re.compile(r"[A-Za-z0-9._/-]+\Z")
PATH_SEGMENT = re.compile(r"[A-Za-z0-9._+][A-Za-z0-9._+-]*\Z")
# The MSI's Directory table gives long names such as "Amazon Redshift ODBC Driver x64", so an
# extract path's segments may also hold a space, written %20, and parentheses.
EXTRACT_SEGMENT = re.compile(r"[A-Za-z0-9._+() ][A-Za-z0-9._+() -]*\Z")
# A file version is VS_FIXEDFILEINFO's four 16-bit fields, compared numerically field by field.
FILE_VERSION = re.compile(r"(0|[1-9][0-9]{0,4})(\.(0|[1-9][0-9]{0,4})){3}\Z")
OFFSET = re.compile(r"(0|[1-9][0-9]{0,9})\Z")
HEX_BYTES = re.compile(r"([0-9a-f]{2})+\Z")
# Windows opens a device, not a file, for these base names whatever the extension.
DEVICES = frozenset(["con", "prn", "aux", "nul"] + ["com%d" % i for i in range(10)] +
                    ["lpt%d" % i for i in range(10)])
# Files the 2.3.0.0 MSI holds that the Windows set must not: what only the Kerberos GSS path
# loads, the driver's configuration and trust files, and its setup tools. The VC++ runtime is a
# host prerequisite, never a set file, as any copy we mapped would serve the whole process.
NOT_IN_SET = ("krbcc64.dll", "leashw64.dll", "xpprof64.dll", "amazon.redshiftodbc.ini", "root.crt",
              "rsodbcsetup.dll", "rsodbcsql.exe")
RUNTIME = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")
# A renamed file gets a generated base name of 16 characters from a-z2-7, except the two whose own
# base name is stored at a fixed length inside them for GetModuleHandleA, which keep its length.
SELF_NAMED = {"krb5_64.dll": 7, "wshelp64.dll": 8}


def segments_ok(segments):
    """No empty segment (so no // and no trailing /), and no . or .. segment."""
    return all(s not in ("", ".", "..") for s in segments)


def https_problem(text):
    """For text that starts with https://: None, "chars" or "segments", the first rule it breaks."""
    rest = text[len("https://"):]
    if not URL_CHARS.match(rest):
        return "chars"
    if not segments_ok(rest.split("/")):
        return "segments"
    return None


def name_problem(name):
    """Why a file name of the set is refused, or None: the rules every row's name follows."""
    if not NAME.match(name):
        return "file name %r must be 1 to 64 characters from A-Za-z0-9._- and not start with . or -" % name
    if name.lower() in RESERVED_NAMES:
        return "file name %r is reserved for what the build writes beside the set" % name
    return None


def version_problem(version):
    if not VERSION.match(version):
        return ("version %r is not four dot-separated numbers without leading zeros (no aliases "
                "such as latest)" % version)
    return None


def check_url(text, version):
    if not text.startswith(URL_PREFIX + version + "/"):
        return "package URL is not under %s%s/" % (URL_PREFIX, version)
    return {
        "chars": "package URL may hold only A-Za-z0-9._/- after https:// (no %, ? or #)",
        "segments": "package URL has an empty, . or .. segment",
    }.get(https_problem(text))


def check_path(text):
    """./seg/seg..., segments from A-Za-z0-9._+-, none empty, . or .., none starting with -."""
    if not text.startswith("./"):
        return "path in the package must start with ./"
    segments = text[2:].split("/")
    if not segments_ok(segments) or not all(PATH_SEGMENT.match(s) for s in segments):
        return ("path in the package must be ./ and segments from A-Za-z0-9._+-, none empty, "
                ". or .. and none starting with -")
    return None


def windows_name_problem(name):
    """Why Windows cannot hold a file of this name, or None: a device name before the first dot, or a
    trailing dot, which Windows strips, so two lock names would open one file."""
    if name.split(".", 1)[0].lower() in DEVICES:
        return "file name %r is a Windows device name" % name
    if name.endswith("."):
        return "file name %r ends with a dot" % name
    return None


def decode_extract_path(text):
    """The path a lock token stands for: %20 is a space, and no other % is allowed."""
    return text.replace("%20", " ") if "%" not in text.replace("%20", "") else None


def check_extract_path(text):
    """./seg/seg..., where the MSI's administrative extract puts the file. A segment Windows would
    change or refuse (a trailing dot or space, a device name) is refused too."""
    path = decode_extract_path(text)
    segments = path[2:].split("/") if path is not None and path.startswith("./") else None
    if segments is None or not segments_ok(segments) or not all(
            EXTRACT_SEGMENT.match(s) and not s.endswith((".", " ")) and not windows_name_problem(s)
            for s in segments):
        return ("extract path must be ./ and segments from A-Za-z0-9._+-(), with a space written "
                "%20, none empty, . or .., none starting with - or ending with a dot or a space, "
                "and none a Windows device name")
    return None


def check_file_version(text):
    if not FILE_VERSION.match(text) or any(int(f) > 65535 for f in text.split(".")):
        return ("minimum file version %r must be four dot-separated decimals from 0 to 65535 "
                "without leading zeros, as VS_FIXEDFILEINFO holds it (10.0.40219.1)" % text)
    return None


def generated_name_problem(name, new_name):
    length = SELF_NAMED.get(name.lower(), 16)
    if not re.match(r"[a-z2-7]{%d}\.dll\Z" % length, new_name):
        return ("the embedded name of %s must be %d characters from a-z2-7 and then .dll, not %r"
                % (name, length, new_name))
    return None


def check_source(text):
    """A notice's source: the https:// URL verification took it from. The build never reads it."""
    if not text.startswith("https://"):
        return ("notice source must be an https:// URL: verification takes every notice from the "
                "release tag, and a package that holds one fails")
    if https_problem(text):
        return ("notice source URL may hold only A-Za-z0-9._/- after https://, with no empty, "
                ". or .. segment")
    return None


DRIVER_STEM = "driver"


def library_stem(name):
    return "library_" + name.lower().replace(".", "_").replace("-", "_")


def notice_stem(name):
    # Names cannot hold anything but A-Za-z0-9._- here, so the stem is a C identifier.
    return "notice_" + name.lower().replace(".", "_").replace("-", "_")


class Errors(list):
    """Every problem found, as "<lock>:<line>: <message>" or "<lock>: <message>"."""

    def __init__(self, path):
        list.__init__(self)
        self.path = path

    def at(self, line_no, message):
        self.append("%s:%d: %s" % (self.path, line_no, message))

    def whole(self, message):
        self.append("%s: %s" % (self.path, message))


def read_rows(data, errors):
    """Splits the lock into header lines (key, value, line_no) and rows (tokens, line_no)."""
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    header, rows = [], []
    for line_no, raw in enumerate(lines, 1):
        bad = [b for b in raw if b != 0x09 and not 0x20 <= b <= 0x7E]
        if bad:
            errors.at(line_no, "byte 0x%02x is not allowed; a lock is printable ASCII, tabs and LF "
                               "line ends" % bad[0])
            continue
        tokens = []
        for token in raw.decode("ascii").split():
            if token.startswith("#"):
                break  # a comment runs to the end of the line; a # inside a token is not one
            tokens.append(token)
        if not tokens:
            continue
        if tokens[0] in ("format", "version", "status"):
            if len(tokens) != 2:
                errors.at(line_no, "%s takes one value" % tokens[0])
                continue
            header.append((tokens[0], tokens[1], line_no))
        else:
            rows.append((tokens, line_no))
    return header, rows


def check_header(header, rows, errors):
    """Returns (version, status), or None when a later rule cannot be checked without them."""
    keys = [k for k, _, _ in header]
    if keys != ["format", "version", "status"]:
        errors.whole("the lock must start with exactly one format, version and status line, in "
                     "that order (found: %s)" % (" ".join(keys) or "none"))
        return None
    first_row = min((n for _, n in rows), default=None)
    if first_row is not None and first_row < header[-1][2]:
        errors.at(first_row, "format, version and status must come before every row")
    fmt, version, status = (v for _, v, _ in header)
    if fmt not in ("1", "2"):
        errors.at(header[0][2], "unknown lock format %r; this checker reads formats 1 and 2" % fmt)
        return None
    problem = version_problem(version)
    if problem:
        errors.at(header[1][2], problem)
        return None
    if status not in ("development", "release"):
        errors.at(header[2][2], "status must be development or release, not %r" % status)
    elif status == "release" and tuple(int(c) for c in version.split(".")) < FIRST_RELEASE:
        errors.at(header[2][2], "a lock below 2.3.0.0 must be status development")
    return fmt, version, status


# Each Windows row kind: its number of fields and its shape, for the message.
WINDOWS_SHAPES = {
    "driver": (8, "windows_amd64 driver <name> <sha256> <source-sha256> <msi-url> <msi-sha256> <extract-path>"),
    "library": (8, "windows_amd64 library <name> <sha256> <source-sha256> <msi-url> <msi-sha256> <extract-path>"),
    "prerequisite": (4, "windows_amd64 prerequisite <name> <minimum file version>"),
    "rename": (4, "windows_amd64 rename <name> <embedded name>"),
    "patch": (6, "windows_amd64 patch <name> <offset> <original bytes> <new bytes>"),
    "name-seed": (3, "windows_amd64 name-seed <sha256>"),
    "set-hash": (3, "windows_amd64 set-hash <sha256>"),
}


def check_windows_row(tokens, version):
    """A format 2 windows_amd64 row: returns (kind, problems) as check_row does."""
    kind = tokens[1]
    fields, shape = WINDOWS_SHAPES[kind]
    if len(tokens) != fields:
        return None, ["a %s row is: %s" % (kind, shape)]
    if kind in RENAME_HASHES:
        return kind, [] if SHA256.match(tokens[2]) else ["a %s is 64 lower-case hex digits" % kind]
    # A rename's embedded name is checked by its own rule, below.
    name = tokens[2]
    problems = [p for p in (name_problem(name) or windows_name_problem(name),) if p]
    if name.lower() in NOT_IN_SET:
        problems.append("the Windows set does not hold %s" % name)
    elif kind in ("driver", "library") and name.lower() in RUNTIME:
        problems.append("%s is a host prerequisite, never a set file" % name)
    if kind in ("driver", "library"):
        for field, value in (("sha256", tokens[3]), ("source sha256", tokens[4]), ("msi sha256", tokens[6])):
            if not SHA256.match(value):
                problems.append("%s must be 64 lower-case hex digits" % field)
        problems += [p for p in (check_url(tokens[5], version), check_extract_path(tokens[7])) if p]
    elif kind == "prerequisite":
        problems += [p for p in (check_file_version(tokens[3]),) if p]
    elif kind == "rename":
        problems += [p for p in (generated_name_problem(tokens[2], tokens[3]),) if p]
        # krbcc64, leashw64 and xpprof64 fit a generated name's alphabet and length.
        if tokens[3].lower() in NOT_IN_SET:
            problems.append("the Windows set does not hold %s" % tokens[3])
    else:
        if not OFFSET.match(tokens[3]):
            problems.append("a patch offset must be decimal without leading zeros, not %r" % tokens[3])
        if not (HEX_BYTES.match(tokens[4]) and HEX_BYTES.match(tokens[5])):
            problems.append("a patch's bytes must be non-empty lower-case hex of even length")
        elif len(tokens[4]) != len(tokens[5]):
            problems.append("a patch's original and new bytes must have the same length")
    return kind, problems


def check_row(tokens, fmt, version):
    """Returns (kind, problems): kind is driver, notice or, in format 2, one of WINDOWS_KINDS, or
    None when the row's shape is wrong."""
    platform, kind = tokens[0], tokens[1] if len(tokens) > 1 else ""
    if platform == WINDOWS and kind in ("driver",) + WINDOWS_KINDS:
        if fmt != "2":
            return None, ["windows_amd64 rows need lock format 2"]
        return check_windows_row(tokens, version)
    if kind == "driver":
        if platform not in PLATFORMS:
            return None, ["unknown platform %r (known: %s)" % (platform, ", ".join(PLATFORMS))]
        if len(tokens) != 7:
            return None, ["a driver row is: <platform> driver <name> <sha256> <package-url> "
                          "<package-sha256> <path-in-package>"]
    elif kind == "notice":
        if platform != "*":
            return None, ["a notice row applies to every platform, so its platform is *"]
        if len(tokens) != 5:
            return None, ["a notice row is: * notice <name> <sha256> <source>"]
    elif fmt == "2" and kind in WINDOWS_KINDS:
        return None, ["a %s row is for windows_amd64" % kind]
    elif fmt == "2":
        return None, ["unknown row; a row is <platform> driver ..., windows_amd64 "
                      "library|prerequisite|rename|patch|name-seed|set-hash ..., or * notice ..."]
    else:
        return None, ["unknown row; a row is <platform> driver ... or * notice ..."]

    name, sha = tokens[2], tokens[3]
    problems = []
    problem = name_problem(name)
    if problem:
        problems.append(problem)
    elif kind == "notice" and name not in REQUIRED_NOTICES:
        problems.append("notice %r is not one of %s, the only notices a set holds"
                        % (name, ", ".join(REQUIRED_NOTICES)))
    if not SHA256.match(sha):
        problems.append("sha256 must be 64 lower-case hex digits")
    if kind == "driver":
        if not SHA256.match(tokens[5]):
            problems.append("package sha256 must be 64 lower-case hex digits")
        problems += [p for p in (check_url(tokens[4], version), check_path(tokens[6])) if p]
    else:
        problems += [p for p in (check_source(tokens[4]),) if p]
    return kind, problems


def check_windows_set(rows, notices, errors):
    """Rules across the windows_amd64 rows: one MSI, unique names ignoring case, and rename and patch
    rows that name a file of the set and explain every difference between its two hashes."""
    files = [(t, n) for t, n in rows["driver"] if t[0] == WINDOWS] + rows["library"]
    if rows["library"] and len(files) == len(rows["library"]):
        errors.at(rows["library"][0][1], "library rows need a windows_amd64 driver row")
    for tokens, line_no in files[1:]:
        if tokens[5:7] != files[0][0][5:7]:
            errors.at(line_no, "%s names another MSI than %s on line %d; the Windows set comes from "
                               "one MSI" % (tokens[2], files[0][0][2], files[0][1]))
    # One namespace, ignoring case as Windows does: the set's files, their embedded names, the
    # notices beside them and the prerequisites the loader looks up by name.
    seen, stems = {}, {}
    by_name = {t[2]: t for t, _ in files}

    def claim(name, line_no, what):
        if name.lower() in seen:
            errors.at(line_no, "%s %s repeats or differs only in case from the name on line %d"
                      % (what, name, seen[name.lower()]))
        seen.setdefault(name.lower(), line_no)

    for tokens, line_no in files:
        claim(tokens[2], line_no, "file")
        stem = DRIVER_STEM if tokens[1] == "driver" else library_stem(tokens[2])
        if stem in stems:
            errors.at(line_no, "%s has the same symbol stem %s as the file on line %d"
                      % (tokens[2], stem, stems[stem]))
        stems.setdefault(stem, line_no)
    for tokens, line_no in notices:
        seen.setdefault(tokens[2].lower(), line_no)
    for tokens, line_no in rows["prerequisite"]:
        claim(tokens[2], line_no, "prerequisite")
    renamed = {}
    for tokens, line_no in rows["rename"]:
        if tokens[2] not in by_name:
            errors.at(line_no, "rename names %s, which is not a windows_amd64 file" % tokens[2])
        elif tokens[2] in renamed:
            errors.at(line_no, "second rename row for %s (first on line %d)" % (tokens[2], renamed[tokens[2]]))
        renamed.setdefault(tokens[2], line_no)
        claim(tokens[3], line_no, "embedded name")
    # The rename's names derive from the name seed, and the set hash names the renamed set's
    # directory, so a renamed lock records each once, and only a renamed lock records them.
    for kind in RENAME_HASHES:
        for tokens, line_no in rows[kind][1:]:
            errors.at(line_no, "second %s row (first on line %d)" % (kind, rows[kind][0][1]))
        if rows["rename"] and not rows[kind]:
            errors.at(rows["rename"][0][1], "a lock with rename rows records its %s row" % kind)
        elif rows[kind] and not rows["rename"]:
            errors.at(rows[kind][0][1], "a %s row needs the rename rows it records" % kind)
    patched = {}
    for tokens, line_no in rows["patch"]:
        if tokens[2] not in by_name:
            errors.at(line_no, "patch names %s, which is not a windows_amd64 file" % tokens[2])
        patched.setdefault(tokens[2], []).append((int(tokens[3]), len(tokens[4]) // 2, line_no))
    # In lock order, as rename_apply.py applies them: each starts at or after the end of the one
    # before it.
    for name, patches in patched.items():
        for (start, size, _), (next_start, _, line_no) in zip(patches, patches[1:]):
            if start + size > next_start:
                errors.at(line_no, "this patch of %s overlaps or precedes the one at offset %d"
                          % (name, start))
    # The patches are the only change between the MSI's bytes and the embedded ones.
    for tokens, line_no in files:
        if tokens[2] in patched and tokens[3] == tokens[4]:
            errors.at(line_no, "%s has patch rows, so its sha256 must differ from its source sha256" % tokens[2])
        elif tokens[2] not in patched and tokens[3] != tokens[4]:
            errors.at(line_no, "%s has no patch row, so its sha256 must equal its source sha256" % tokens[2])


def check_sets(rows, notices, errors):
    """Rules across rows: one driver per platform, macOS rows alike but for the platform, and no two
    names or stems in one set alike."""
    drivers = rows["driver"]
    seen_platforms = {}
    for tokens, line_no in drivers:
        if tokens[0] in seen_platforms:
            errors.at(line_no, "second driver row for %s (first on line %d)"
                      % (tokens[0], seen_platforms[tokens[0]]))
        seen_platforms.setdefault(tokens[0], line_no)
    # A platform's set is its driver, its libraries and their embedded names, plus every notice. A
    # case-insensitive file system would merge two names that differ only in case, and equal stems
    # would collide in the assembler.
    set_names = {t[2].lower(): n for t, n in drivers + rows["library"]}
    set_names.update((t[3].lower(), n) for t, n in rows["rename"])
    seen_names, seen_stems = {}, {DRIVER_STEM: 0}
    for tokens, line_no in notices:
        name = tokens[2]
        if name.lower() in set_names:
            errors.at(line_no, "notice %s has the name of a file of the set" % name)
        elif name.lower() in seen_names:
            errors.at(line_no, "notice %s repeats or differs only in case from the notice on "
                               "line %d" % (name, seen_names[name.lower()]))
        elif notice_stem(name) in seen_stems:  # guards a later change to REQUIRED_NOTICES
            errors.at(line_no, "notice %s has the same symbol stem %s as another notice"
                      % (name, notice_stem(name)))
        seen_names.setdefault(name.lower(), line_no)
        seen_stems.setdefault(notice_stem(name), line_no)
    # Both macOS platforms embed the one universal library of the one universal package, which
    # verification checks once and prints both rows of, so a lock whose two rows differ elsewhere
    # holds bytes no single verification approved.
    macos = [(t, n) for t, n in drivers if t[0] in ("osx_amd64", "osx_arm64")]
    if len(macos) == 2 and macos[0][0][1:] != macos[1][0][1:]:
        errors.at(macos[1][1], "the %s row differs from the %s row on line %d in more than its platform; both "
                               "name the one universal library of the one universal package"
                  % (macos[1][0][0], macos[0][0][0], macos[0][1]))
    check_windows_set(rows, notices, errors)
    if not drivers and not errors:
        errors.whole("the lock has no driver row")
    named = {t[2] for t, _ in notices}
    missing = [n for n in REQUIRED_NOTICES if n not in named]
    if missing and not errors:
        errors.whole("the lock has no notice row for %s" % ", ".join(missing))


def normalise(fmt, version, status, rows, notices):
    out = ["format " + fmt, "version " + version, "status " + status]
    for t, _ in rows["driver"]:
        if t[0] == WINDOWS:
            out.append(" ".join(["driver", t[0], t[2], t[3], DRIVER_STEM] + t[5:8] + [t[4]]))
        else:
            out.append(" ".join(["driver", t[0], t[2], t[3], DRIVER_STEM, t[4], t[5], t[6]]))
    for t, _ in rows["library"]:
        out.append(" ".join(["library", t[0], t[2], t[3], library_stem(t[2])] + t[5:8] + [t[4]]))
    for kind in WINDOWS_KINDS[1:]:
        out += [" ".join([kind, t[0]] + t[2:]) for t, _ in rows[kind]]
    for t, _ in notices:
        out.append(" ".join(["notice", t[2], t[3], notice_stem(t[2]), t[4]]))
    return out


def check(path):
    """Returns (normalised lines, []) for a good lock, or (None, every problem found)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        return None, ["%s: cannot read the lock: %s" % (path, e.strerror)]
    errors = Errors(path)
    header, rows = read_rows(data, errors)
    checked = check_header(header, rows, errors)
    if checked is None:
        return None, list(errors)
    fmt, version, status = checked
    by_kind = dict((k, []) for k in ("driver", "notice") + WINDOWS_KINDS)
    for tokens, line_no in rows:
        kind, problems = check_row(tokens, fmt, version)
        for p in problems:
            errors.at(line_no, p)
        if kind and not problems:
            by_kind[kind].append((tokens, line_no))
    notices = by_kind.pop("notice")
    check_sets(by_kind, notices, errors)
    if errors:
        return None, list(errors)
    return normalise(fmt, version, status, by_kind, notices), []


def driver_name_problem(name):
    # A driver file named like a notice is refused here as check_sets refuses it in a lock.
    problem = name_problem(name)
    if problem is None and name.lower() in (r.lower() for r in REQUIRED_NOTICES):
        problem = "file name %r is a notice's" % name
    return problem


def main(argv):
    # One part of a lock by itself, by the same rules, for verification: the version asked for, and
    # the driver's path and file name from its policy file.
    single = {
        "--version": version_problem,
        "--path": check_path,
        "--name": driver_name_problem,
    }
    if len(argv) == 3 and argv[1] in single:
        problem = single[argv[1]](argv[2])
        if problem:
            sys.stderr.write("check-lock: %r: %s\n" % (argv[2], problem))
            return 1
        return 0
    if len(argv) != 2:
        sys.stderr.write("usage: lock.py <lock> | lock.py --version <version> | "
                         "lock.py --path <path-in-package> | lock.py --name <file-name>\n")
        return 2
    out, errors = check(argv[1])
    if errors:
        for e in errors:
            sys.stderr.write("check-lock: %s\n" % e)
        return 1
    # As bytes, so a native Windows Python writes LF too: the readers do not strip a CR.
    sys.stdout.buffer.write(("\n".join(out) + "\n").encode())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
