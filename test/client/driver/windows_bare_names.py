"""The rewritten-string check, on Windows: the strings the rename wrote
over the bare-name loads of the DLLs the set leaves out cannot be file names, so a LoadLibraryA of
each maps nothing, even with a fake under the original name beside it, and no file of such a name
can be created for a load to find.

On Windows Server 2022 such a load fails with ERROR_MOD_NOT_FOUND (126), where ERROR_INVALID_NAME
(123) might be expected, so either passes: what the check is for is that nothing is mapped and
nothing can be. That the name can never name a file is shown on its own, without resting on a
load's error code: creating a file of that name in the same folder must fail too.

    python3 -I windows_bare_names.py names <lock>
    python3 -I windows_bare_names.py check <lock> <plant directory> <fake file>
    python3 -I windows_bare_names.py judge <lock> <results> <plant directory>

The strings come from the lock itself, not from a list of their own: a windows_amd64 `patch` row
whose original bytes are the bare name of a `*.dll` with no `driver` or `library` row of its own
records one, since the set leaves that DLL out. The lock as it is written and the normalised form
`check-lock.sh` prints put the platform and the kind the other way round, and both are read
(`krbcc64.dll` and `leashw64.dll` in the 2.3.0.0 lock, which `krb5_64.dll` and `gssapi64.dll`
load through their own function tables).

names prints one line per rewritten string, `<original name> <the string as lowercase hex>`, so the
rule can be read off Windows.
check, on Windows: copies <fake file> into <plant directory> under each original name, so a bare
name does reach a file there, then from that directory loads each original name (the control, which
must map the fake) and each rewritten string (which must not), and judges the outcomes.
judge applies check's rules to a results file, so they run anywhere. Each line is
`<the name as lowercase hex> <kind> <outcome>`: for `original` (the control's load) and
`rewritten` (a rewritten string's load) the outcome is `invalid-name`, `loaded <path>` or
`error <number>`; for `name` (a file of a rewritten string, created in <plant directory>) it is
`refused`, `exists` (a file of that name is already there) or `created` (and removed again):

  names      the lock records at least one rewritten string, each as long as the name it replaced,
             and no '|' rewrite in a form the reader does not load (the control: a lock whose patch
             rows the reader does not understand fails rather than passes), and the results hold
             exactly one line per rewritten string, one per original name and one per name's
             creation;
  control    each original name was loaded, from a file in <plant directory>;
  refused    each rewritten string mapped nothing: ERROR_INVALID_NAME (123), which refuses the name
             outright, or ERROR_MOD_NOT_FOUND (126), which found no file of it;
  name       no file of a rewritten string can be created in <plant directory>, nor is one there,
             so no planted file could ever answer such a load.

On failure it prints one line, "<rule>: <reason>", to stderr and exits 1. Standard library only.
"""

import ntpath
import os
import re
import shutil
import sys

#: The platform whose rows are read, in either of the two row orders below.
PLATFORM = "windows_amd64"
#: ERROR_INVALID_NAME: "The filename, directory name, or volume label syntax is incorrect."
ERROR_INVALID_NAME = 123
#: ERROR_MOD_NOT_FOUND: Windows read the string as a name and found no file of it.
ERROR_MOD_NOT_FOUND = 126
#: The outcomes of a load that mapped nothing, either of which this check takes.
NOT_MAPPED = ("invalid-name", "error %d" % ERROR_MOD_NOT_FOUND)
#: The longest name a load is tried with: a rewritten string is as long as the name it replaced.
MAX_NAME = 260


class Failure(Exception):
    pass


#: A run the rename writes over a name: three or more '|' characters, in ASCII or as UTF-16LE.
#: Shorter runs are not taken for a name: a changed header field or address can hold 0x7c by chance.
BAR_RUN = re.compile(rb"\|{3,}|(?:\|\x00){2,}\|")


def bar_runs(original, new):
    """The '|' runs a patch row wrote over other bytes, anywhere in the row: the pin step joins
    changes a few bytes apart into one row, so a rewritten name can share a row with a rename or a
    changed field next to it, and the row's bytes as a whole are then no name."""
    return [m.group() for m in BAR_RUN.finditer(new) if original[m.start() : m.end()] != m.group()]


def rewritten_strings(lock_text):
    """[(original name, rewritten bytes)] of each windows_amd64 patch row that rewrote a bare DLL
    name the set leaves out, in the lock's order, one row per distinct rewritten string: the string
    is what a load is tried with, so a second name the rename wrote the same string over (two names
    of one length give one string) adds no load, and the row keeps the first such name for its
    control and its message. A row that wrote a run of '|' characters over anything else (a name
    without .dll, a UTF-16LE one, a name sharing its row with other changed bytes) fails: the
    rename writes those too, and such a string must not drop out of the suite unloaded."""
    rows = []
    for line in lock_text.splitlines():
        fields = line.split("#", 1)[0].split()
        # The lock file writes "<platform> <kind> ...", check-lock.sh's normalised output
        # "<kind> <platform> ...", and the callers hand over either, so both are read.
        if len(fields) > 2 and fields[0] == PLATFORM:
            rows.append(fields[1:])
        elif len(fields) > 2 and fields[1] == PLATFORM:
            rows.append([fields[0]] + fields[2:])
    files = set(fields[1].lower() for fields in rows if fields[0] in ("driver", "library"))
    found, already = [], set()
    for fields in rows:
        # patch <file> <offset> <original hex> <new hex>
        if fields[0] != "patch" or len(fields) != 5:
            continue
        try:
            original, new = bytes.fromhex(fields[3]), bytes.fromhex(fields[4])
        except ValueError:
            raise Failure("names: %s has a patch row whose bytes are not hex" % fields[1])
        text = original.decode("latin-1")
        if not text.lower().endswith(".dll") or text.lower() in files:
            if bar_runs(original, new):
                raise Failure(
                    "names: %s's patch row at %s wrote '|' over %s, which this reader does not load (it "
                    "loads an ASCII name ending in .dll that names no set file)" % (fields[1], fields[2], fields[3])
                )
            continue  # a set member's name, or not a name at all (padding, a changed header field)
        if new not in already:
            already.add(new)
            found.append((text, new))
    return found


def names(lock_text):
    """The rewritten strings, checked: at least one, each as long as the name it replaced and no
    longer than a name can be, and each original a plain file name."""
    found = rewritten_strings(lock_text)
    if not found:
        raise Failure(
            "names: the lock records no patch row that rewrote a bare name the set leaves out, so "
            "there is no rewritten string to load; it is read from the windows_amd64 patch rows"
        )
    for original, new in found:
        if not new or len(new) > MAX_NAME or len(new) != len(original):
            raise Failure(
                "names: the string that replaced %s is %d bytes long, and it must be as long as the name, %d bytes"
                % (original, len(new), len(original))
            )
        # check plants a fake under the original name, so a row's name must be a plain file name:
        # not a path (either separator), not drive-relative, and no longer than a name can be.
        if original != ntpath.basename(original) or ntpath.splitdrive(original)[0] or len(original) > MAX_NAME:
            raise Failure("names: the lock rewrote %r, which is not a plain file name" % original)
    return found


def load_outcome(name):
    """The outcome of a LoadLibraryA of `name` in this process; Windows only."""
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryA.argtypes = [wintypes.LPCSTR]
    kernel32.LoadLibraryA.restype = wintypes.HMODULE
    kernel32.GetModuleFileNameA.argtypes = [wintypes.HMODULE, ctypes.c_char_p, wintypes.DWORD]
    kernel32.GetModuleFileNameA.restype = wintypes.DWORD
    ctypes.set_last_error(0)
    module = kernel32.LoadLibraryA(name)
    error = ctypes.get_last_error()
    if not module:
        return "invalid-name" if error == ERROR_INVALID_NAME else "error %d" % error
    path = ctypes.create_string_buffer(32768)
    # A module with no readable path still counts as loaded: the rule is that nothing was mapped.
    read = kernel32.GetModuleFileNameA(module, path, len(path))
    return "loaded %s" % (path.value.decode("latin-1") if read else "(its path could not be read)")


def create_outcome(name):
    """Whether a file of `name` can be created in the current folder: "refused" when the system
    will not take the name, "exists" when a file of that name is already there, else "created",
    and the file is removed again."""
    path = name.decode("latin-1")
    try:
        with open(path, "xb"):
            pass
    except FileExistsError:
        # The name is a file name after all: one is there, and a load could find it.
        return "exists"
    except (OSError, ValueError):
        # OSError: the file system refused the name; ValueError: the runtime did (an embedded NUL).
        return "refused"
    try:
        os.unlink(path)
    except OSError:
        pass
    return "created"


def comparable(path):
    """`path` as Windows compares it: backslashes, no \\\\?\\ prefix, normalised, case-insensitive."""
    if path.replace("/", "\\").startswith("\\\\?\\"):
        path = path[4:]
    return ntpath.normcase(ntpath.normpath(path))


def judge(lock_text, results, plant_directory):
    """check's rules on `results`, a list of `<hex> <kind> <outcome>` lines."""
    found = names(lock_text)
    want = {}
    for original, new in found:
        want[(new.hex(), "rewritten")] = original
        want[(new.hex(), "name")] = original
        want[(original.encode("latin-1").hex(), "original")] = original
    seen = {}
    for line in results:
        fields = line.split(None, 2)
        if not fields:
            continue
        if len(fields) < 3 or fields[1] not in ("rewritten", "original", "name"):
            raise Failure("names: %r is not <hex> <rewritten|original|name> <outcome>" % line)
        key = (fields[0].lower(), fields[1])
        if key not in want:
            raise Failure("names: the results hold a %s name the lock does not record: %s" % (fields[1], fields[0]))
        if key in seen:
            raise Failure("names: the results hold %s %s twice" % (fields[1], want[key]))
        seen[key] = fields[2]
    missing = sorted("%s %s" % (kind, want[(h, kind)]) for h, kind in want if (h, kind) not in seen)
    if missing:
        raise Failure("names: the results hold no line for %s" % ", ".join(missing))
    # The control first: a refusal below means nothing unless a bare name does reach a file here.
    order = ("original", "rewritten", "name")
    for key, outcome in sorted(seen.items(), key=lambda pair: (order.index(pair[0][1]), pair[0][0])):
        name = want[key]
        if key[1] == "original":
            if not outcome.startswith("loaded "):
                raise Failure(
                    "control: a load of the planted %s was %s, not loaded, so a bare name reaches "
                    "no file here and a refusal below proves nothing" % (name, outcome)
                )
            path = outcome[len("loaded ") :]
            if comparable(ntpath.dirname(path)) != comparable(plant_directory):
                raise Failure(
                    "control: the planted %s loaded from %s, not from %s, so the fake beside the "
                    "load is not the file that answered" % (name, path, plant_directory)
                )
        elif key[1] == "name":
            if outcome != "refused":
                raise Failure(
                    "name: a file of the string that replaced %s could be created or is there (%s), so "
                    "the string can name a file and a planted one could answer such a load" % (name, outcome)
                )
        elif outcome not in NOT_MAPPED:
            raise Failure(
                "refused: a load of the string that replaced %s was %s, and it must map nothing: "
                "ERROR_INVALID_NAME (%d) or ERROR_MOD_NOT_FOUND (%d)"
                % (name, outcome, ERROR_INVALID_NAME, ERROR_MOD_NOT_FOUND)
            )
    print(
        "windows_bare_names: each of the %d rewritten strings maps nothing and can name no file, "
        "while a fake under each original name in %s loads" % (len(found), plant_directory)
    )


def check(lock_text, plant_directory, fake):
    """check's command, on Windows: plants the fake under each original name and loads each name."""
    if os.name != "nt":
        raise Failure("windows: check runs on Windows only; judge applies its rules anywhere")
    found = names(lock_text)
    results, here = [], os.getcwd()
    for original, _ in found:
        shutil.copyfile(fake, os.path.join(plant_directory, original))
    os.chdir(plant_directory)
    try:
        # The control first: with the fake planted here, the original name does load.
        for original, new in found:
            results.append(
                "%s original %s" % (original.encode("latin-1").hex(), load_outcome(original.encode("latin-1")))
            )
            results.append("%s rewritten %s" % (new.hex(), load_outcome(new)))
            results.append("%s name %s" % (new.hex(), create_outcome(new)))
    finally:
        os.chdir(here)
    for line in results:
        print(line)
    judge(lock_text, results, plant_directory)


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "names":
            for original, new in names(read(argv[2])):
                print("%s %s" % (original, new.hex()))
        elif len(argv) == 5 and argv[1] == "check":
            check(read(argv[2]), argv[3], argv[4])
        elif len(argv) == 5 and argv[1] == "judge":
            with open(argv[3], encoding="ascii", errors="replace") as f:
                judge(read(argv[2]), [line.rstrip("\r\n") for line in f], argv[4])
        else:
            sys.stderr.write(
                "usage: windows_bare_names.py names <lock>\n"
                "       windows_bare_names.py check <lock> <plant directory> <fake file>\n"
                "       windows_bare_names.py judge <lock> <results> <plant directory>\n"
            )
            return 2
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except OSError as e:
        sys.stderr.write("file: %s\n" % e)
        return 1
    return 0


def read(path):
    with open(path, encoding="ascii", errors="replace") as f:
        return f.read()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
