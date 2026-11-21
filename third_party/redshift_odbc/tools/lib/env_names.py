"""The environment-name rule of verification.

    python3 -I env_names.py <policy-dir> <driver> <platform>

<driver> is the driver, or a directory whose regular files are all scanned, as Windows verification
passes the set's DLLs: every name is then found in the set as a whole, and a new name's problem
names the files that hold it. Lists the names in <driver> that its code could pass to getenv: every
run of A-Za-z0-9_ that ends a NUL-terminated string, and every tail of such a run, because the
linker may store a literal as the tail of a longer one (2.3.0.0 holds "https_proxy" only inside
"idp_use_https_proxy"). Of those it keeps each PG name (^PG[A-Z_]+$, without the PGRES_
result-status names) and each name that starts with a prefix in environment-prefixes.txt and
continues in the prefix's case (A-Z0-9_ after an upper-case prefix, a-z0-9_ after a lower-case one,
as environment variable names are written; SSL_CTX_new is a function, not a variable). It fails,
printing every problem, when:

- a found name is in none of refused-environment.txt, known-environment.txt and
  ignored-strings.txt, so a new string, variable or not, fails until a reviewer classifies it;
- a listed name or string that the scan would report is not found, so no row outlives the string it
  was made for. A refused row is exempt only when its comment says why: "# libpq variable, refused
  even where absent", which only the twelve libpq variables the refused list always holds may claim
  (PGGSSLIB, which 2.3.0.0 lacks, is one), "# built at run time from <string>" (a name the driver
  builds from <string>, which must be found), or "# only in <platform>,..." (a name only some
  platforms' drivers hold, which must be found on those), optionally followed by "; <reason>". A
  known or ignored row may carry "# only in <platform>,..." too, and then must not be found on any
  other platform, so it classifies the string only where a reviewer saw it. A refused row's list
  only exempts it from this rule: every refused name is refused on every platform, since
  driver_info.h compiles them all in for each one, which fails closed. Each listed platform, and
  <platform> itself, must be one the lock accepts, so a misspelt list exempts nothing;
- a row is malformed: prefixes are all upper or all lower case; refused rows are
  <NAME> <load|handle|connect>; known rows <NAME> <Keyword> or <NAME> - <reason>; ignored rows
  <string> <reason>, for a string the scan would report; a name in two rows or two lists.

The scan sees only literal names under those prefixes; a review of the driver's source finds the
rest. A policy file or the driver that cannot be read stops the run at once, with the problems found
so far and no comparison, which would only list names the missing input should have held. macOS
verification runs it on each slice of the universal library, which lib/macho.py thin writes out, as
that slice's platform (osx_amd64 for x86_64, osx_arm64 for arm64). Standard library only, Python 3.6
and later.
"""

import os
import re
import sys

ENV_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
PG_NAME = re.compile(r"PG[A-Z_]+\Z")
KEYWORD = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
PREFIX = re.compile(r"[A-Z0-9_]+\Z|[a-z0-9_]+\Z")
UPPER_REST = re.compile(r"[A-Z0-9_]*\Z")
LOWER_REST = re.compile(r"[a-z0-9_]*\Z")
PHASES = ("load", "handle", "connect")
REQUIRED = "libpq variable, refused even where absent"
# The libpq variables the refused list always holds, which may therefore be absent from a driver:
# the list is fixed here, so a typo or a stale row cannot claim the exemption.
REQUIRED_NAMES = ("PGGSSLIB", "PGHOSTADDR", "PGKRBSRVNAME", "PGOPTIONS", "PGREQUIRESSL",
                  "PGSERVICE", "PGSERVICEFILE", "PGSSLCERT", "PGSSLCRL", "PGSSLKEY",
                  "PGSSLROOTCERT", "PGSYSCONFDIR")
BUILT = re.compile(r"built at run time from ([A-Za-z0-9_]+)\Z")
ONLY = re.compile(r"only in ([a-z0-9_,]+)(?:; .+)?\Z")
PLATFORMS = ("linux_amd64", "linux_arm64", "osx_amd64", "osx_arm64", "windows_amd64")


def scanned(name, prefixes):
    """Whether the scan reports name: a PG name or a name under a prefix, in its case."""
    if PG_NAME.match(name):
        return not name.startswith("PGRES_")
    for p in prefixes:
        if name.startswith(p):
            rest = UPPER_REST if p.upper() == p else LOWER_REST
            if rest.match(name[len(p):]):
                return True
    return False


def only_in(comment, at, problems):
    """The platforms of a "# only in" comment, or None. A list naming anything but the lock's
    platforms is a problem, and exempts nothing: a misspelt platform would exempt the row
    everywhere."""
    match = ONLY.match(comment)
    if not match:
        return None
    platforms = match.group(1).split(",")
    if any(p not in PLATFORMS for p in platforms):
        problems.append("policy: %s: \"only in\" names platforms from %s, separated by commas"
                        % (at, ", ".join(PLATFORMS)))
        return None
    return platforms


class Unreadable(Exception):
    """A policy file or the driver cannot be read: the problem is recorded, and the scan stops,
    since every comparison after it would report names the missing input should have held."""


def rows(path, problems):
    """(tokens, comment, line) for each line; a # starting a token starts the comment."""
    out = []
    try:
        with open(path, encoding="ascii") as f:
            for n, line in enumerate(f, 1):
                tokens, comment = [], ""
                for i, token in enumerate(line.split()):
                    if token.startswith("#"):
                        comment = " ".join(line.split()[i:])[1:].strip()
                        break
                    tokens.append(token)
                if tokens:
                    out.append((tokens, comment, "%s:%d" % (os.path.basename(path), n)))
    except (OSError, UnicodeDecodeError) as e:
        problems.append("policy: cannot read %s: %s" % (path, e))
        raise Unreadable() from e
    return out


def read_policy(policy, problems):
    prefixes = []
    for tokens, _, at in rows(os.path.join(policy, "environment-prefixes.txt"), problems):
        if len(tokens) != 1 or not PREFIX.match(tokens[0]):
            problems.append("policy: %s: a row is one prefix, all upper or all lower case, from "
                            "A-Za-z0-9_" % at)
        else:
            prefixes.append(tokens[0])
    lists, seen, exempt, scope = {}, {}, {}, {}

    def add(name, kind, at):
        if name in seen:
            problems.append("policy: %s: %s is already classified at %s" % (at, name, seen[name]))
        seen[name] = at
        lists[name] = kind

    for tokens, comment, at in rows(os.path.join(policy, "refused-environment.txt"), problems):
        if len(tokens) != 2 or not ENV_NAME.match(tokens[0]) or tokens[1] not in PHASES:
            problems.append("policy: %s: a refused row is <NAME> <load|handle|connect>" % at)
            continue
        add(tokens[0], "refused", at)
        if comment == REQUIRED or BUILT.match(comment) or only_in(comment, at, problems):
            exempt[tokens[0]] = comment
    for tokens, comment, at in rows(os.path.join(policy, "known-environment.txt"), problems):
        platforms = only_in(comment, at, problems)
        if platforms:
            exempt[tokens[0]], scope[tokens[0]] = comment, platforms
        if not ENV_NAME.match(tokens[0]):
            problems.append("policy: %s: %s is not a variable name" % (at, tokens[0]))
        elif len(tokens) == 2 and KEYWORD.match(tokens[1]):
            add(tokens[0], "known", at)
        elif len(tokens) >= 3 and tokens[1] == "-":
            add(tokens[0], "known", at)
        else:
            problems.append("policy: %s: a known row is <NAME> <Keyword>, or <NAME> - <reason>; %s "
                            "has neither a keyword nor a reason" % (at, tokens[0]))
    for tokens, comment, at in rows(os.path.join(policy, "ignored-strings.txt"), problems):
        platforms = only_in(comment, at, problems)
        if platforms:
            exempt[tokens[0]], scope[tokens[0]] = comment, platforms
        if len(tokens) < 2:
            problems.append("policy: %s: an ignored row is <string> <reason>; %s has no reason"
                            % (at, tokens[0]))
        elif not scanned(tokens[0], prefixes):
            problems.append("policy: %s: %s is not a string the scan reports, so it needs no row"
                            % (at, tokens[0]))
        else:
            add(tokens[0], "ignored", at)
    return prefixes, lists, seen, exempt, scope


def scan(data, prefixes):
    """Every name the scan reports: each tail of a NUL-terminated run of A-Za-z0-9_ that starts
    with PG or a prefix, wherever it starts, and keeps the prefix's case."""
    rest = re.compile(rb"[A-Za-z0-9_]*\x00")
    found = set()
    for start in set(prefixes) | {"PG"}:
        token = start.encode("ascii")
        at = data.find(token)
        while at >= 0:
            end = rest.match(data, at + len(token))
            if end:
                name = data[at:end.end() - 1].decode("ascii")
                if scanned(name, prefixes):
                    found.add(name)
            at = data.find(token, at + 1)
    return found


def read_driver(path, problems):
    """{file name: bytes} of the driver, or of each regular file of the directory `path`."""
    try:
        names = sorted(os.listdir(path)) if os.path.isdir(path) else [None]
    except OSError as e:
        problems.append("environment: cannot read %s: %s" % (path, e.strerror))
        raise Unreadable() from e
    files = {}
    for name in names:
        where = path if name is None else os.path.join(path, name)
        try:
            if name is not None and (os.path.islink(where) or not os.path.isfile(where)):
                raise OSError(0, "not a regular file")
            with open(where, "rb") as f:
                files[name] = f.read()
        except OSError as e:
            problems.append("environment: cannot read %s: %s" % (where, e.strerror))
            raise Unreadable() from e
    return files


def stale_rows(lists, found, prefixes, exempt, seen, platform, scanned_what="the driver"):
    """The rows whose name the driver, or the set (`scanned_what`), no longer holds, and is not
    excused from holding."""
    problems = []
    for name in sorted(lists):
        if not scanned(name, prefixes) or name in found:
            continue
        reason = exempt.get(name, "")
        built, only = BUILT.match(reason), ONLY.match(reason)
        if reason == REQUIRED:
            if name in REQUIRED_NAMES:
                continue
            problems.append("environment: %s (%s) is not one of the libpq variables the refused "
                            "list always holds, so it cannot be absent from the driver as one; "
                            "remove its row" % (name, seen[name]))
            continue
        if built and name.startswith(built.group(1)) and name != built.group(1):
            # The string it is built from must still be in the driver. The scan already reports
            # every string under a prefix, and the base of a scanned name is scanned too, so being
            # among the names found is the whole test: a base the linker stored only as the tail of
            # a longer string counts, as the name built from it would.
            if built.group(1) in found:
                continue
            problems.append("environment: %s (%s) is built from %s, which is no longer in %s"
                            % (name, seen[name], built.group(1), scanned_what))
            continue
        if only and platform not in only.group(1).split(","):
            continue
        problems.append("environment: %s (%s) is no longer in %s; remove its row"
                        % (name, seen[name], scanned_what))
    return problems


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: env_names.py <policy-dir> <driver> <platform>\n")
        return 2
    problems = []
    platform = argv[3]
    if platform not in PLATFORMS:
        problems.append("environment: %s is not a platform (%s)" % (platform, ", ".join(PLATFORMS)))
    try:
        prefixes, lists, seen, exempt, scope = read_policy(argv[1], problems)
        files = read_driver(argv[2], problems)
    except Unreadable:
        for p in problems:
            sys.stderr.write("%s\n" % p)
        return 1
    held = {name: scan(data, prefixes) for name, data in files.items()}
    found = set().union(*held.values())
    scanned_what = "the driver" if None in held else "the set"
    for name in sorted(found):
        holder = scanned_what if None in held else "the set's " + ",".join(
            f for f in sorted(held) if name in held[f])
        if name not in lists:
            problems.append("environment: %s names %s, which no policy list classifies; add "
                            "it to refused-environment.txt or known-environment.txt, or, if it is "
                            "not a variable, to ignored-strings.txt" % (holder, name))
        elif name in scope and platform not in scope[name]:
            problems.append("environment: %s names %s, whose row (%s) is only in %s; add %s "
                            "to the row if this driver holds it for the same reason"
                            % (holder, name, seen[name], ",".join(scope[name]), platform))
    problems += stale_rows(lists, found, prefixes, exempt, seen, platform, scanned_what)
    if problems:
        for p in problems:
            sys.stderr.write("%s\n" % p)
        return 1
    count = {k: sum(1 for n in found if lists[n] == k) for k in ("refused", "known", "ignored")}
    sys.stdout.write("%s's %d scanned names are all classified (%d refused, %d known, %d ignored)\n"
                     % (scanned_what, len(found), count["refused"], count["known"], count["ignored"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
