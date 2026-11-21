"""The configuration files the driver opens or probes on macOS, from an fs_usage trace of macos-hosts.sh's test
host, which loads the driver and runs the closed-port smoke: every path the process named must be a row of
policy/config-files.txt or one of the kinds of file below that configure nothing.

    python3 -I macos_config_files.py <trace> <config-files.txt> <driver> <home> <cwd> <process> <control> <ca-file>

<trace> is `fs_usage -w -f pathname <process>`'s output; <driver> the librsodbc64.dylib the host loaded, beside
which `<driver-dir>/` rows are, in a directory whose parent holds the host's own files, which are not counted;
<home> and <cwd> the host's HOME and current directory, outside that parent; <control> a path no row lists that
the host probed first, which must be found and is not counted; <ca-file> the CA file the smoke names as CaFile,
as the ODBC client always names one, which is the connection's own: it must be found, showing the smoke reached
the driver's CA check, and is not counted. Prints the rows the trace met and every unlisted path; exits 0 when
the trace met exactly `<driver-dir>/amazon.redshiftodbc.ini` and `<driver-dir>/root.crt` (as on Linux: before a
connection the driver opens or probes only its own settings file, and seeing it shows the trace sees the driver's
own calls; with a CA file named, driver 2.3.1.0 also looks for its built-in root.crt beside itself), the control,
the CA file and no unlisted path, 1 otherwise, 2 on a usage error. A path on the data volume
(/System/Volumes/Data/...) is read as the path it backs. Only a path fs_usage prints whole from the root is
classified: a relative one, or one it shortened from its start, is not, and one it cut short at its end is read as
written, but for the system cryptex's prefix cut short (CRYPTEXES, below).
"""

import fnmatch
import os
import re
import sys

# Where macOS keeps what /etc, /tmp and /var name: a row names the first, a trace may show either.
FIRMLINKS = (("/private/etc/", "/etc/"), ("/private/tmp/", "/tmp/"), ("/private/var/", "/var/"))
# The writable data volume, which holds what the root's firmlinks name (/Users, /private/var, /usr/local and the
# rest): a trace may name a file by its path there, which is read as the path it backs, so it is classified as that.
DATA_VOLUME = "/System/Volumes/Data"
# What the process reads that configures nothing: the system's libraries, frameworks and data, the dyld cache,
# devices, the system's read-only databases and its build-environment probe (/AppleInternal), Rosetta 2's runtime
# and translation cache (an osx_amd64 process), and /proc, which macOS lacks and where code written for Linux looks
# for the process's own state. Under /System/, only the sealed system volume's files: not /System/Volumes/
# (VOLUMES), where the other volumes, the data volume among them, are mounted, but for CRYPTEXES.
VOLUMES = "/System/Volumes/"
# Apple's signed system cryptex on the Preboot volume, from which macOS 13 and later serve the system's frameworks
# and libraries (/System/Cryptexes/OS names it): read-only and sealed, like the system volume. fs_usage may cut a
# path there short at its end (/System/Volumes/Preboot/Cryptex): a path past PREBOOT that is the start of CRYPTEXES
# is read as one under it.
PREBOOT = "/System/Volumes/Preboot/"
CRYPTEXES = PREBOOT + "Cryptexes/"
SYSTEM = (
    "/System/",
    "/usr/lib/",
    "/usr/share/",
    "/Library/Apple/",
    "/dev/",
    "/private/var/db/timezone/",
    "/var/db/timezone/",
    "/private/var/db/dyld/",
    "/var/db/dyld/",
    "/AppleInternal/",
    "/usr/libexec/rosetta/",
    "/private/var/db/oah/",
    "/var/db/oah/",
    "/proc/",
)
# CoreFoundation's per-user text encoding, which it reads from the account's own home (not HOME) in any process
# that links it, as the driver does.
CORE_FOUNDATION = "/.CFUserTextEncoding"


def rows(config_files, home, cwd, driver_dir):
    """Each non-Windows row as (fnmatch pattern, its location as written). The <kerberos-prefix>/ rows name
    nothing here: the macOS driver links the system's Kerberos framework and bundles no Kerberos library."""
    out = []
    for line in open(config_files, encoding="utf-8"):
        fields = line.split()
        if not fields or fields[0].startswith("#") or "\\" in fields[0] or fields[0].startswith("<$"):
            continue
        pattern = fields[0]
        if pattern.startswith("<kerberos-prefix>/"):
            continue
        for prefix, value in (("~/", home + "/"), ("./", cwd + "/"), ("<driver-dir>/", driver_dir + "/")):
            if pattern.startswith(prefix):
                pattern = value + pattern[len(prefix) :]
        pattern = re.sub(r"%\{[^}]*\}|%[a-z]+", "*", pattern)
        out.append((pattern + "*" if pattern.endswith("/") else pattern, fields[0]))
    return out


def paths(trace, process):
    """The absolute paths the traced process named: on each line of that process (its last field is
    <process>.<thread>), the field that starts with / and what follows it up to the elapsed time."""
    found = set()
    for line in open(trace, encoding="utf-8", errors="replace"):
        fields = line.split()
        if len(fields) < 3 or not fields[-1].startswith(process + "."):
            continue
        start = next((i for i, f in enumerate(fields[1:-1], 1) if f.startswith("/")), None)
        if start is None:
            continue
        end = start + 1
        while end < len(fields) - 1 and not re.fullmatch(r"[0-9.]+|W", fields[end]):
            end += 1
        found.add(" ".join(fields[start:end]))
    return found


def on_root(path):
    """A path on the data volume as the path it backs: /System/Volumes/Data/Users/x is /Users/x."""
    if path == DATA_VOLUME or path.startswith(DATA_VOLUME + "/"):
        return path[len(DATA_VOLUME) :] or "/"
    return path


def cryptex(path):
    """Under the system cryptex, or its prefix cut short past PREBOOT."""
    return path.startswith(CRYPTEXES) or (
        path.startswith(PREBOOT) and len(path) > len(PREBOOT) and CRYPTEXES.startswith(path)
    )


def canonical(path):
    for long, short in FIRMLINKS:
        if path.startswith(long):
            return short + path[len(long) :]
    return path


def main(argv):
    if len(argv) != 9:
        sys.stderr.write(__doc__)
        return 2
    trace, config_files, driver, home, cwd, process, control, ca_file = argv[1:]
    driver_dir = os.path.dirname(driver)
    patterns = rows(config_files, home, cwd, driver_dir)
    own = os.path.dirname(driver_dir)  # the driver directory's parent, holding the host's own files
    met, unlisted, saw_control, saw_ca = set(), [], False, False
    for traced in sorted(paths(trace, process)):
        path = on_root(traced)
        short = canonical(path)
        if short in (control, canonical(control)):
            saw_control = True
            continue
        if short in (ca_file, canonical(ca_file)):
            saw_ca = True
            continue
        listed = [
            location
            for pattern, location in patterns
            if fnmatch.fnmatchcase(short, pattern) or fnmatch.fnmatchcase(path, pattern)
        ]
        met.update(listed)
        mine = path == own or path.startswith(own + "/") or own.startswith(path + "/")
        if (
            listed
            or mine
            or (path.startswith(SYSTEM) and not path.startswith(VOLUMES))
            or cryptex(path)
            or path.endswith(CORE_FOUNDATION)
            or path in ("/", "/private", "/var", "/tmp")
        ):
            continue
        unlisted.append(traced)
    print("rows the trace met: %s" % (", ".join(sorted(met)) or "none"))
    for path in unlisted:
        print("unlisted: %s" % path)
    ini, built_in = "<driver-dir>/amazon.redshiftodbc.ini", "<driver-dir>/root.crt"
    ok = met == {ini, built_in} and saw_control and saw_ca and not unlisted
    if not saw_control:
        print("the trace does not show the control path %s, so it may not show the process's other calls" % control)
        with open(trace, encoding="utf-8", errors="replace") as lines:
            print("the trace begins: " + " | ".join(line.strip()[:160] for line in list(lines)[:4]))
    if not saw_ca:
        print(
            "the trace does not show the CA file the smoke names, %s, so the smoke may not have reached the "
            "driver's CA check" % ca_file
        )
    if ini not in met:
        print("the trace does not show the driver's own probe of amazon.redshiftodbc.ini")
    if built_in not in met:
        print("the trace does not show the driver's look for its built-in %s beside the CA file" % built_in)
    if met - {ini, built_in}:
        print("before a connection the driver opened or probed rows other than its own %s and %s" % (ini, built_in))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
