"""Takes one regular file out of a cpio archive.

    python3 -I rpm.py --payload <package> | python3 -I cpio.py <path-in-archive> <out-file>

Reads the archive on stdin, in the "newc" format an RPM payload holds, to its end, and then makes
<out-file>, which must not exist yet, hold the data of the one member at <path-in-archive>. Until
the whole archive has been checked that data is only in a private temporary beside <out-file>, so
<out-file> appears only for a valid archive, and no other member, link, device or directory reaches
the file system, whatever the archive holds. It fails, leaving no <out-file>, unless exactly one
member has that path and that member is a regular file with one link, and unless the whole archive
is well formed: every member's path is spelled ./<segment>/..., with no empty, . or .. segment
after the first (so it is not absolute either), or is the lone . root directory a macOS payload
starts with, which names no path; every header field is hexadecimal, every name ends with one NUL,
sizes and counts are within bounds, the trailer is empty, and at most a block of zero
padding follows it. Paths are compared without their empty and . segments, so <path-in-archive>
may be written opt/x.so or ./opt/x.so alike. The "crc" format is refused, because its checksums are
not verified here. Every member is checked, so a bad one fails the extraction even after the wanted
member was found.

    python3 -I rpm.py --payload <package> | python3 -I cpio.py --find <name>...

checks the archive the same way and prints "<name> <path>", one line per member of any type whose
last path segment is one of the names, and writes nothing. Verification uses it to find the notices
a package holds, which fail the run. Standard library only, Python 3.6 and later.
"""

import os
import re
import stat
import sys
import tempfile

MAGIC = b"070701"  # newc
HEADER_SIZE = 110
HEX_FIELDS = re.compile(rb"[0-9A-Fa-f]{104}\Z")
TRAILER = b"TRAILER!!!"
CHUNK = 1 << 20
MAX_NAME = 4096
MAX_FILE = 1 << 30  # far above any driver file so far (2.3.0.0's largest, the macOS dylib, is 39.2 MB)
MAX_MEMBERS = 100000
MAX_TOTAL = 4 << 30
MAX_PADDING = 4096  # GNU cpio pads to 512-byte blocks; an RPM payload has none


def read_exact(f, n):
    data = f.read(n)
    if len(data) != n:
        raise ValueError("the archive ends inside a member")
    return data


def skip(f, n):
    while n:
        n -= len(read_exact(f, min(n, CHUNK)))


def padding(n):
    return (4 - n % 4) % 4  # headers with their names, and data, are padded to four bytes


def normal(path):
    """The path with its empty and . segments dropped, so every spelling of one path compares equal;
    a leading / is kept, so an absolute path never equals a relative one."""
    segments = [s for s in path.split(b"/") if s not in (b"", b".")]
    return (b"/" if path.startswith(b"/") else b"") + b"/".join(segments)


def canonical(name):
    """./ then one or more segments, none empty, . or .., the one spelling rpmbuild writes. A member
    spelled otherwise could name the locked path without being counted as a second member of it.
    A lone . is the root directory a macOS payload starts with; it names no path, so it is allowed."""
    if name == b".":
        return True
    first, _, rest = name.partition(b"/")
    return first == b"." and all(s not in (b"", b".", b"..") for s in rest.split(b"/"))


def members(archive):
    """Reads the archive to its end, checking every member and the trailer, and yields
    (name, mode, nlink, size, copy) for each member; copy(out) writes its data to the file out, and
    a member's data is skipped if copy is not called."""
    members = total = 0
    while True:
        header = read_exact(archive, HEADER_SIZE)
        if header[:6] == b"070702":
            raise ValueError("the archive is in cpio's crc format, which is not accepted")
        if header[:6] != MAGIC or not HEX_FIELDS.match(header[6:]):
            raise ValueError("not a newc cpio archive, or a header field is not hexadecimal")
        field = [int(header[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        mode, nlink, size, name_size = field[1], field[4], field[6], field[11]
        if not 1 <= name_size <= MAX_NAME:
            raise ValueError("a member's name size %d is out of bounds" % name_size)
        raw_name = read_exact(archive, name_size)
        if raw_name[-1:] != b"\0" or b"\0" in raw_name[:-1]:
            raise ValueError("a member's name does not end with exactly one NUL")
        name = raw_name[:-1]
        skip(archive, padding(HEADER_SIZE + name_size))
        if name == TRAILER:
            if size != 0:
                raise ValueError("the archive's trailer is not empty")
            break
        if name.startswith(b"/") or b".." in name.split(b"/"):
            raise ValueError("member %r has an absolute path or a .. segment" % name)
        if not canonical(name):
            raise ValueError("member %r is not spelled ./<path> with no empty or . segment" % name)
        members += 1
        total += size
        # Each bound is checked on the header, before the member's data is read.
        if size > MAX_FILE:
            raise ValueError("member %r declares %d bytes, more than the %d this reader accepts"
                             % (name, size, MAX_FILE))
        if members > MAX_MEMBERS:
            raise ValueError("the archive has more than %d members" % MAX_MEMBERS)
        if total > MAX_TOTAL:
            raise ValueError("the archive's members hold more than %d bytes" % MAX_TOTAL)
        copied = []

        def copy(out, left=size):
            while left:
                chunk = read_exact(archive, min(left, CHUNK))
                out.write(chunk)
                left -= len(chunk)
            copied.append(True)

        yield name, mode, nlink, size, copy
        if not copied:
            skip(archive, size)
        skip(archive, padding(size))
    # Only a bounded run of zero padding may follow the trailer, so nothing can hide after it.
    after = archive.read(MAX_PADDING + 1)
    if after.strip(b"\0"):
        raise ValueError("data follows the archive's trailer")
    if len(after) > MAX_PADDING:
        raise ValueError("more than %d bytes follow the archive's trailer" % MAX_PADDING)


def extract(archive, wanted, out_path):
    wanted_name = normal(wanted.encode("utf-8", "surrogateescape"))
    created = False
    temporary = None
    try:
        for name, mode, nlink, _, copy in members(archive):
            if normal(name) != wanted_name:
                continue
            if created:
                raise ValueError("the archive has more than one member at %s" % wanted)
            if not stat.S_ISREG(mode):
                raise ValueError("the member at %s is not a regular file (mode %o)" % (wanted, mode))
            if nlink != 1:
                raise ValueError("the member at %s has %d links, not one" % (wanted, nlink))
            out_dir = os.path.dirname(os.path.abspath(out_path))
            fd, temporary = tempfile.mkstemp(prefix=".cpio.", dir=out_dir)
            created = True
            with os.fdopen(fd, "wb") as out:
                copy(out)
        if not created:
            raise ValueError("the archive has no member at %s" % wanted)
        os.link(temporary, out_path)  # fails if <out-file> exists, and never follows a link there
    finally:
        if temporary is not None:
            os.unlink(temporary)


def find(archive, names):
    """The path of every member, of any type, whose last segment is one of names, ignoring case as
    the lock's notice names do, as (name, path) pairs; a matching path must be printable ASCII
    without spaces."""
    wanted = set(n.encode("ascii").lower() for n in names)
    found = []
    for path, _, _, _, _ in members(archive):
        last = path.rsplit(b"/", 1)[-1]
        if last.lower() not in wanted:
            continue
        if not all(0x21 <= b <= 0x7E for b in path):
            raise ValueError("member %r has a space or a byte that is not printable ASCII" % path)
        found.append((last.decode("ascii"), path.decode("ascii")))  # printable ASCII, checked
    return found


def main(argv):
    if len(argv) >= 3 and argv[1] == "--find":
        try:
            for name, path in find(sys.stdin.buffer, argv[2:]):
                sys.stdout.write("%s %s\n" % (name, path))
        except (ValueError, OSError) as e:
            sys.stderr.write("cpio.py: %s\n" % e)
            return 1
        return 0
    if len(argv) != 3:
        sys.stderr.write("usage: cpio.py <path-in-archive> <out-file> < archive\n"
                         "       cpio.py --find <name>... < archive\n")
        return 2
    try:
        extract(sys.stdin.buffer, argv[1], argv[2])
    except (ValueError, OSError) as e:
        sys.stderr.write("cpio.py: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
