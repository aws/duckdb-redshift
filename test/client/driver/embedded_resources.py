"""Checks that each Windows image holds the embedded driver set as its RCDATA resources.

    python3 -I embedded_resources.py <driver_info.h> <image>...

The set is the header's file list, which fetch-driver.sh checked against the lock: each file must
be one resource, named by its symbol stem in upper case as the resource script names it, with the
size and SHA-256 the header records, and the image may hold no other RCDATA resource. make
test_driver's Windows branch runs it on six images that link the loader: the loadable extension,
the shell, unittest, duckdb.dll, the client suite and the driver suite (not DuckDB's
plan_serializer.exe, which links it too). Prints one line per image that passes; otherwise one line
per problem on stderr, and exits 1. Reads the images with third_party/redshift_odbc/tools/lib/pe.py.
Standard library only.
"""

import hashlib
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES_LIST = "#define REDSHIFT_DRIVER_FILES(X) \\"
FILES_ROW = re.compile(r'\tX\("([^"]+)", ([0-9]+), "([0-9a-f]{64})", ([A-Za-z_][A-Za-z0-9_]*)\)( \\)?\Z')


def load_pe():
    # By its file, so nothing else on sys.path is searched, and with no bytecode written beside it.
    sys.dont_write_bytecode = True
    path = os.path.join(HERE, "..", "..", "..", "third_party", "redshift_odbc", "tools", "lib", "pe.py")
    spec = importlib.util.spec_from_file_location("redshift_odbc_pe", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def set_files(header):
    """{resource label: (file name, size, SHA-256)} from the header's file list, or None."""
    lines = header.splitlines()
    if FILES_LIST not in lines:
        return None
    files = {}
    for line in lines[lines.index(FILES_LIST) + 1 :]:
        row = FILES_ROW.match(line)
        if not row:
            return None
        # Keyed as pe.rcdata labels a string-named resource, a JSON-quoted string, so the two
        # dictionaries compare by key.
        files[json.dumps(row.group(4).upper())] = (row.group(1), int(row.group(2)), row.group(3))
        if not row.group(5):
            return files
    return None


def problems(pe, files, image_path):
    with open(image_path, "rb") as f:
        data = f.read()
    found = {}
    for label, language, size, at in pe.rcdata(pe.Image(data)):
        found.setdefault(label, []).append((language, size, at))
    out = ["RCDATA %s is not a file of the set" % label for label in found if label not in files]
    for label, (name, size, sha) in files.items():
        entries = found.get(label, [])
        if len(entries) != 1:
            out.append("RCDATA %s (%s) is there %d times, not once" % (label, name, len(entries)))
            continue
        _, got_size, at = entries[0]
        got_sha = hashlib.sha256(data[at : at + got_size]).hexdigest()
        if (got_size, got_sha) != (size, sha):
            out.append(
                "RCDATA %s (%s) is %d bytes with SHA-256 %s; driver_info.h records %d bytes with %s"
                % (label, name, got_size, got_sha, size, sha)
            )
    return out


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: embedded_resources.py <driver_info.h> <image>...\n")
        return 2
    try:
        with open(argv[1], encoding="ascii") as f:
            files = set_files(f.read())
    except (OSError, UnicodeDecodeError) as e:
        sys.stderr.write("embedded_resources: cannot read %s: %s\n" % (argv[1], e))
        return 1
    if not files:
        sys.stderr.write("embedded_resources: %s has no complete REDSHIFT_DRIVER_FILES list\n" % argv[1])
        return 1
    pe = load_pe()
    failed = False
    for image in argv[2:]:
        try:
            found = problems(pe, files, image)
        except pe.Failure as e:
            found = [str(e)]
        except OSError as e:
            found = ["cannot read it: %s" % e.strerror]
        for problem in found:
            sys.stderr.write("embedded_resources: %s: %s\n" % (image, problem))
        if found:
            failed = True
        else:
            print(
                "embedded_resources: %s holds the %d files of the set, as driver_info.h records them"
                % (image, len(files))
            )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
