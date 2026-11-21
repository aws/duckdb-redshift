"""Generates driver_info.h for one platform, and the Windows resource script.

    check-lock.sh | python3 -I driver_info.py <platform> <set-dir> <policy-dir>
    python3 -I driver_info.py resources <template> <driver_info.h> <out.rc>

tools/fetch-driver.sh runs it last, with the lock it read through check-lock.sh on stdin, so it
never sees a lock that failed check-lock.sh, and the lock is checked once per fetch. <set-dir> holds
the platform's driver set, the driver, on Windows its libraries, and every notice, under the names
they are embedded by (a Windows file's rename row gives its embedded name), where the assembler or
the resource compiler will read them; each file's size comes from the file, and its SHA-256 must
equal the lock's, as the set hash they give must equal a set-hash row's. The header names each file
by its absolute path in <set-dir>, with symbolic links resolved, because the assembler looks for a
bare .incbin name in its working directory before its include path; any byte a path holds is
escaped, so any build directory works. On success it writes <set-dir>/driver_info.h,
through an exclusively created temporary and one rename; on any failure it prints every problem to
stderr and writes nothing.

resources fills the template's one @REDSHIFT_DRIVER_RESOURCES@ line with an RCDATA statement for
each file of the header's embed list, named by its stem in upper case and given by its full path
with forward slashes. The resource compiler reads a path in the build's code page and a backslash
as an escape, so a path that is not printable ASCII, or holds a double quote, is refused. It writes
<out.rc> as the header is written. Standard library only, Python 3.6 and later.
"""

import hashlib
import os
import re
import stat
import sys
import tempfile

ENV_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
KEYWORD = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
LIBRARY = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._+-]{0,63}\Z")
PHASES = ("load", "handle", "connect")
OS_OF_PLATFORM = {"linux_amd64": "linux", "linux_arm64": "linux", "osx_amd64": "macos",
                  "osx_arm64": "macos", "windows_amd64": "windows"}


def policy_rows(path, errors):
    """(tokens, line_no) for each non-comment line. A # starting a token starts a comment."""
    rows = []
    try:
        with open(path, encoding="ascii") as f:
            for line_no, line in enumerate(f, 1):
                tokens = []
                for token in line.split():
                    if token.startswith("#"):
                        break
                    tokens.append(token)
                if tokens:
                    rows.append((tokens, line_no))
    except (OSError, UnicodeDecodeError) as e:
        errors.append("%s: cannot read the policy file: %s" % (path, e))
    return rows


def read_policy(policy_dir, os_name, errors):
    def error(path, line_no, message):
        errors.append("%s:%d: %s" % (path, line_no, message))

    refused, seen = [], {}
    path = os.path.join(policy_dir, "refused-environment.txt")
    for tokens, n in policy_rows(path, errors):
        if len(tokens) != 2 or not ENV_NAME.match(tokens[0]) or tokens[1] not in PHASES:
            error(path, n, "a row is <NAME> <load|handle|connect>, NAME from A-Za-z0-9_")
        elif tokens[0] in seen:
            error(path, n, "%s is listed twice" % tokens[0])
        else:
            seen[tokens[0]] = n
            refused.append((tokens[0], tokens[1]))

    overridden = []
    path = os.path.join(policy_dir, "known-environment.txt")
    for tokens, n in policy_rows(path, errors):
        name = tokens[0]
        if not ENV_NAME.match(name) or len(tokens) < 2:
            error(path, n, "a row is <NAME> <Keyword>, or <NAME> - <reason>")
        elif tokens[1] == "-":
            if len(tokens) < 3:
                error(path, n, "%s: a - row needs a reason" % name)
        elif len(tokens) != 2 or not KEYWORD.match(tokens[1]):
            error(path, n, "%s: the keyword must be one word from A-Za-z0-9_" % name)
        if name in seen:
            error(path, n, "%s is listed twice, or is in both lists" % name)
        seen[name] = n
        if len(tokens) == 2 and KEYWORD.match(tokens[1]) and ENV_NAME.match(name):
            overridden.append((name, tokens[1]))

    prerequisites = []
    path = os.path.join(policy_dir, "host-prerequisites.txt")
    for tokens, n in policy_rows(path, errors):
        if tokens[0] == "windows" and len(tokens) > 2:
            # The minimum file version has one home, the lock, which the loader reads.
            error(path, n, "a windows row names the DLL only; its minimum file version is in the lock")
        elif len(tokens) != 2 or tokens[0] not in ("linux", "macos", "windows") or not LIBRARY.match(tokens[1]):
            error(path, n, "a row is <linux|macos|windows> <library file name>")
        elif tokens[0] == os_name:
            prerequisites.append(tokens[1])
    return refused, overridden, prerequisites


def file_facts(set_dir, name, want_sha, errors):
    """Size and SHA-256 of <set-dir>/<name>, read without following a link."""
    path = os.path.join(set_dir, name)
    try:
        # O_NONBLOCK, so a FIFO is refused below rather than waiting for a writer; Windows has
        # neither FIFOs nor O_NONBLOCK, and needs O_BINARY so the bytes hashed are the file's.
        flags = getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
        fd = os.open(path, os.O_RDONLY | flags)
    except OSError as e:
        errors.append("%s: cannot open (%s); a set file must be a regular file, not a link"
                      % (path, e.strerror))
        return None
    with os.fdopen(fd, "rb") as f:
        if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
            errors.append("%s: not a regular file" % path)
            return None
        digest, size = hashlib.sha256(), 0
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    sha = digest.hexdigest()
    if sha != want_sha:
        errors.append("%s: SHA-256 %s, the lock expects %s" % (path, sha, want_sha))
        return None
    return size


def c_string(text):
    # Every value has passed check-lock.sh or the policy grammar, so none holds " or \.
    assert '"' not in text and "\\" not in text
    return '"%s"' % text


SAFE_PATH_BYTES = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._+-")


def path_string(path):
    """A C and assembler string literal for any path: other bytes as three-digit octal escapes,
    which GNU as, LLVM's assembler and the C compiler all read back as the same bytes."""
    return '"%s"' % "".join(chr(b) if b in SAFE_PATH_BYTES else "\\%03o" % b for b in os.fsencode(path))


def generate(platform, set_dir, policy_dir, lock_text):
    errors = []
    embed_dir = os.path.realpath(set_dir)
    lines = lock_text.splitlines()
    version = status = None
    driver, libraries, notices, renames, minimums, set_hash = None, [], [], {}, [], None
    for line in lines:
        f = line.split(" ")
        if f[0] == "version":
            version = f[1]
        elif f[0] == "status":
            status = f[1]
        elif f[0] == "driver" and f[1] == platform:
            driver = (f[2], f[3], f[4])  # name, sha256, stem
        elif f[0] == "library" and f[1] == platform:
            libraries.append((f[2], f[3], f[4]))
        elif f[0] == "rename" and f[1] == platform:
            renames[f[2]] = f[3]
        elif f[0] == "prerequisite" and f[1] == platform:
            minimums.append((f[2], f[3]))
        elif f[0] == "set-hash" and f[1] == platform:
            set_hash = f[2]
        elif f[0] == "notice":
            notices.append((f[1], f[2], f[3]))
    if platform not in OS_OF_PLATFORM:
        return None, ["unknown platform %r" % platform]
    if version is None or status is None:
        return None, ["the lock on stdin is not check-lock.sh output"]
    if driver is None:
        return None, ["the lock has no driver row for %s, so that platform is not built with a "
                      "driver" % platform]

    files = []
    for name, sha, stem in [driver] + libraries + notices:
        name = renames.get(name, name)  # a renamed file's sha256 is of its embedded bytes
        size = file_facts(set_dir, name, sha, errors)
        files.append((name, size, sha, stem))
    windows = OS_OF_PLATFORM[platform] == "windows"
    refused, overridden, prerequisites = read_policy(policy_dir, OS_OF_PLATFORM[platform], errors)
    # The driver loads DbgHelp.dll by bare name when runtime-names.txt lists it, with or without
    # .dll, which LoadLibrary adds; then the loader checks that copy. A reviewed change to
    # runtime-names.txt, not this script, decides the list.
    runtime_names = os.path.join(policy_dir, "windows", "runtime-names.txt")
    system_dbghelp = windows and any(" ".join(tokens[1:]).lower() in ("dbghelp", "dbghelp.dll")
                                     for tokens, _ in policy_rows(runtime_names, errors))
    # The policy names each Windows prerequisite and the lock gives its minimum version, so each
    # must name the same DLLs, ignoring case as Windows does.
    listed, locked = set(p.lower() for p in prerequisites), set(n.lower() for n, _ in minimums)
    errors += ["the lock gives a minimum version for %s, which policy/host-prerequisites.txt does "
               "not list" % n for n, _ in minimums if n.lower() not in listed]
    errors += ["policy/host-prerequisites.txt lists %s, whose minimum version the lock does not "
               "give" % p for p in prerequisites if windows and p.lower() not in locked]
    if errors:
        return None, errors

    # The manifest names the private directory: one line per file, sorted by name, so the set hash
    # does not depend on the order of the lock's rows.
    manifest = "".join("%s  %d  %s\n" % (sha, size, name)
                       for name, size, sha, _ in sorted(files, key=lambda f: f[0].encode()))
    set_sha = hashlib.sha256(manifest.encode("ascii")).hexdigest()
    # A renamed lock records the set hash its pin computed, which the fetched set must give.
    if set_hash not in (None, set_sha):
        return None, ["the set's manifest has SHA-256 %s, and the lock's set-hash is %s"
                      % (set_sha, set_hash)]

    def listing(items):
        return ", ".join(items)

    out = [
        "/* Generated by third_party/redshift_odbc/tools/fetch-driver.sh from the driver lock and",
        "   the policy files, for %s. Do not edit. Also read by the assembler, so it holds only" % platform,
        "   preprocessor definitions and C comments. */",
        "#pragma once",
        "#define REDSHIFT_DRIVER_VERSION %s" % c_string(version),
        "#define REDSHIFT_DRIVER_PLATFORM %s" % c_string(platform),
        "#define REDSHIFT_DRIVER_DEVELOPMENT %d" % (1 if status == "development" else 0),
        "#define REDSHIFT_DRIVER_SET_SHA256 %s" % c_string(set_sha),
        "/* X(name, size in bytes, SHA-256, assembler symbol stem): the driver, %sthen the notices. */"
        % ("the libraries, " if windows else ""),
        "#define REDSHIFT_DRIVER_FILES(X) \\",
    ]
    out += ["\tX(%s, %d, %s, %s)%s" % (c_string(name), size, c_string(sha), stem,
                                       " \\" if i + 1 < len(files) else "")
            for i, (name, size, sha, stem) in enumerate(files)]
    out += [
        "/* Every refused name, whatever its phase; then the load-phase ones the loader checks. */",
        "#define REDSHIFT_DRIVER_REFUSED_ENVIRONMENT %s" % listing(c_string(n) for n, _ in refused),
        "#define REDSHIFT_DRIVER_LOAD_REFUSED_ENVIRONMENT %s"
        % listing(c_string(n) for n, phase in refused if phase == "load"),
        "/* {variable, the connection-string keyword that overrides it} */",
        "#define REDSHIFT_DRIVER_OVERRIDDEN_ENVIRONMENT %s"
        % listing("{%s, %s}" % (c_string(n), c_string(k)) for n, k in overridden),
        "#define REDSHIFT_DRIVER_HOST_PREREQUISITES %s" % listing(c_string(p) for p in prerequisites),
    ]
    if windows:
        out += [
            "/* X(DLL name, the four fields of its minimum file version) */",
            "#define REDSHIFT_DRIVER_PREREQUISITE_VERSIONS(X) %s"
            % " ".join("X(%s, %s)" % (c_string(n), v.replace(".", ", ")) for n, v in minimums),
            "/* 1 when policy/windows/runtime-names.txt lists DbgHelp.dll, a load by bare name */",
            "#define REDSHIFT_DRIVER_SYSTEM_DBGHELP %d" % (1 if system_dbghelp else 0),
        ]
    out += [
        "/* X(assembler symbol stem, the absolute path the assembler embeds the file from) */",
        "#define REDSHIFT_DRIVER_EMBED_FILES(X) \\",
    ]
    out += ["\tX(%s, %s)%s" % (stem, path_string(os.path.join(embed_dir, name)),
                              " \\" if i + 1 < len(files) else "")
            for i, (name, _, _, stem) in enumerate(files)]
    return "\n".join(line.rstrip() for line in out) + "\n", []


PLACEHOLDER = "@REDSHIFT_DRIVER_RESOURCES@"
EMBED_LIST = "#define REDSHIFT_DRIVER_EMBED_FILES(X) \\"
EMBED_ROW = re.compile(r'\tX\(([A-Za-z_][A-Za-z0-9_]*), "((?:[^"\\]|\\[0-7]{3})*)"\)( \\)?\Z')


def resource_script(template, header):
    """The template with one RCDATA statement per row of the embed list generate() wrote: X(stem,
    path_string()), each row but the last ending in a backslash."""
    lines = header.splitlines()
    if EMBED_LIST not in lines or template.splitlines().count(PLACEHOLDER) != 1:
        return None, ["the header has no embed list, or the template has no line %s alone" % PLACEHOLDER]
    statements = []
    for line in lines[lines.index(EMBED_LIST) + 1:]:
        row = EMBED_ROW.match(line)
        if not row:
            return None, ["the header's embed list has a row that is not X(stem, path): %r" % line]
        path = re.sub(rb"\\([0-7]{3})", lambda m: bytes([int(m.group(1), 8)]), row.group(2).encode("ascii"))
        if any(b < 0x20 or b > 0x7E or b == ord('"') for b in path):
            return None, ["%s: the resource compiler reads a path only in printable ASCII with no double quote; "
                          "build in a directory whose path is printable ASCII with no double quote"
                          % path.decode("ascii", "backslashreplace")]
        statements.append('%s RCDATA "%s"' % (row.group(1).upper(), path.decode("ascii").replace("\\", "/")))
        if not row.group(3):
            return template.replace(PLACEHOLDER, "\n".join(statements)), []
    return None, ["the header's embed list has no last row"]


def main(argv):
    if len(argv) == 5 and argv[1] == "resources":
        try:
            with open(argv[2], encoding="ascii") as template, open(argv[3], encoding="ascii") as header:
                text, errors = resource_script(template.read(), header.read())
        except (OSError, UnicodeDecodeError) as e:
            text, errors = None, ["cannot read the template or the header: %s" % e]
        out = argv[4]
    elif len(argv) == 4:
        platform, set_dir, policy_dir = argv[1:]
        text, errors = generate(platform, set_dir, policy_dir, sys.stdin.read())
        out = os.path.join(set_dir, "driver_info.h")
    else:
        sys.stderr.write("usage: driver_info.py <platform> <set-dir> <policy-dir> < checked-lock\n"
                         "       driver_info.py resources <template> <driver_info.h> <out.rc>\n")
        return 2
    if errors:
        for e in errors:
            sys.stderr.write("driver_info: %s\n" % e)
        return 1
    # An exclusively created temporary with a leading dot, a name no set file can have
    # (check-lock.sh), renamed over the output: never a partial file, never through a link.
    fd, temporary = tempfile.mkstemp(prefix="." + os.path.basename(out) + ".", dir=os.path.dirname(out) or ".")
    try:
        with os.fdopen(fd, "w", encoding="ascii") as f:
            f.write(text)
        os.replace(temporary, out)
    except BaseException:
        os.unlink(temporary)
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
