"""Writes a newc cpio archive for the tool tests: the member checks' fixture writer, used by cpio_test.sh and,
wrapped in test-signed packages, by the verification suites. Standard library only.

    python3 -I make_cpio.py <out> [<form>] <member>...

<out> - is standard output. Each member is name:kind[:nlink[:data-file]], kind one of file, link,
dir, fifo or dev, including the types a normal user cannot create. Its data is the bytes of
<data-file>, or "data of <name>". A first argument starting with ! changes the archive's form
instead: !crc (the crc magic), !magic (the old portable magic), !badhex (a header field that is not
hexadecimal), !nonul (names without their NUL), !innernul (a NUL inside each name), !zeroname (a
name size of 0 in the first header), !after (a member after the trailer), !short (truncated),
!padding (too much padding after the trailer), !trailer (a trailer with data), !declare=<n> (the
first member declares <n> bytes and the archive ends there), !many=<n> (<n> empty files before the
members) and !total (four members of 1 GiB of zeros before the members, streamed, so it is meant
for - and a pipe).
"""

import stat
import sys

KINDS = {
    "file": stat.S_IFREG | 0o644,
    "link": stat.S_IFLNK | 0o777,
    "dir": stat.S_IFDIR | 0o755,
    "fifo": stat.S_IFIFO | 0o644,
    "dev": stat.S_IFCHR | 0o644,
}


MAGIC = {"!crc": b"070702", "!magic": b"070707"}
GiB = 1 << 30


def header(form, name, mode, nlink, size):
    ends = {"!nonul": b"", "!innernul": b"\0x\0"}.get(form, b"\0") if name != "TRAILER!!!" else b"\0"
    raw = name.encode() + ends
    fields = [0, mode, 0, 0, nlink, 0, size, 0, 0, 0, 0, len(raw), 0]
    head = MAGIC.get(form, b"070701") + b"".join(b"%08X" % v for v in fields)
    if form == "!badhex":
        head = head[:20] + b"zz" + head[22:]
    return head + raw + b"\0" * ((4 - (110 + len(raw)) % 4) % 4)


def member(form, name, mode, nlink, data):
    blob = header(form, name, mode, nlink, len(data))
    return blob + data + b"\0" * ((4 - len(data) % 4) % 4)


def main(argv):
    out, specs = argv[1], argv[2:]
    form = specs.pop(0) if specs and specs[0].startswith("!") else ""
    sink = open(sys.stdout.fileno(), "wb", closefd=False) if out == "-" else open(out, "wb")
    with sink as f:
        if form == "!zeroname":
            # The fields of a file with an empty name, then the name size 0; nothing else.
            f.write(header(form, "", KINDS["file"], 1, 0)[:94] + b"%08X%08X" % (0, 0))
            return
        if form.startswith("!declare="):
            f.write(header(form, "./opt/big", KINDS["file"], 1, int(form[9:])))
            return
        for i in range(int(form[6:]) if form.startswith("!many=") else 0):
            f.write(member(form, "./m/%d" % i, KINDS["file"], 1, b""))
        if form == "!total":
            zeros = bytes(1 << 20)
            for i in range(4):
                f.write(header(form, "./zeros/%d" % i, KINDS["file"], 1, GiB))
                for _ in range(GiB // len(zeros)):
                    f.write(zeros)
        for spec in specs:
            parts = spec.split(":")
            name, kind = parts[0], parts[1]
            nlink = int(parts[2]) if len(parts) > 2 else 1
            if len(parts) > 3:
                with open(parts[3], "rb") as d:
                    data = d.read()
            else:
                data = ("data of " + name).encode()
            f.write(member(form, name, KINDS[kind], nlink, data))
        trailer = member(form, "TRAILER!!!", 0, 1, b"x" if form == "!trailer" else b"")
        blob = trailer + b"\0" * (5000 if form == "!padding" else 300)
        if form == "!after":
            blob += member(form, "./opt/x.so", KINDS["file"], 1, b"hidden")
        f.write(blob)
    if form == "!short":
        with open(out, "r+b") as f:
            f.truncate(150)


if __name__ == "__main__":
    main(sys.argv)
