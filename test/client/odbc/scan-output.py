#!/usr/bin/env python3
"""Fail on a credential in captured output: the ODBC client's output scan.

    scan-output.py [--canary VALUE]... FILE...

Reads each FILE line by line and reports, as FILE:LINE: WHAT, every access-key ID,
every assignment of a credential keyword whose value is not a redaction marker, and every
canary VALUE. It never prints the text it found, so the report cannot spread a leaked secret. Exits
0 when nothing is found, 1 when something is, and 2 for a usage error or a file it cannot read.
Run it over every log of a run with live credentials.

Redaction's other two rules, runs of 40 token characters (connect phase) and bound values of 8 bytes
or more (statement phase), need the phase and the values, so a whole log cannot be read for them: a
run passes canaries in their place. The scan does not read SQL literals either, so the quoted literal
redaction replaces after `SECRET_ACCESS_KEY`, `SESSION_TOKEN`, `MASTER_SYMMETRIC_KEY` or `PASSWORD`
is not reported. A marker before an escaped line end (`\\n` in a repr or a JSON string) reads as
covering part of a value and is reported, as are a shell's `PWD=/path` and a URL whose scheme is a
keyword (`pwd://h`), so a log given to the scan holds none of them. A line holding a NUL byte, as
UTF-16 text does, is unreadable.
"""

import argparse
import re
import sys

# The forms, as diagnostic_redaction.cpp reads them: one of its keywords no word runs
# on into, case-insensitive, an optional closing quote (plain, escaped or doubled), `=` or `:` amid
# blanks. A keyword inside a longer one, as `token` in `web_identity_token`, is not its own.
ACCESS_KEY = re.compile(rb"(?<![A-Za-z0-9_])(?:AKIA|ASIA)[A-Z0-9]{16}(?![A-Za-z0-9_])")
ASSIGNMENT = re.compile(
    rb"(?<![A-Za-z0-9_])(aws_secret_access_key|aws_session_token|AccessKeyID|SecretAccessKey|SessionToken"
    rb"|session_token|Password|PWD|web_identity_token|token|client_secret|ProxyPwd|https_proxy_password"
    rb"|master_symmetric_key)(?:\\+[\"']|\"\"|''|[\"'])?[ \t\f\v]*([=:])[ \t\f\v]*",
    re.IGNORECASE,
)
# Redaction's marker ends the value it replaced: only `;`, a cut's closing `...` or the line's end
# follows it, as HoldsValue reads it, or the value's own closing quote or brace and then the
# setting's end. Any other text is a value the marker covers only part of.
MARKER = re.compile(rb"[{\"']?<(?:redacted|param [0-9]+)>(?:[}\"']?(?:;|[\r\n]*$)|\.\.\.[\r\n]*$|[}\"'][,}])")


def holds_value(line, match):
    """Whether the assignment `match` found in `line` has a value, as diagnostic_redaction.cpp reads it.

    A colon right before a colon is a cast; a separator with no value before the line or the setting
    ends assigns nothing. A keyword's own colon before `//` is reported, though redaction reads it as a
    URL's scheme: the sanitizer shows that URL as `<url>`, and a secret can start with `//`.
    """
    rest = line[match.end() :]
    if match.group(2) == b":" and match.end() == match.end(2) and rest.startswith(b":"):
        return False
    if MARKER.match(rest):
        return False
    return rest.strip(b"\r\n") != b"" and not rest.startswith(b";")


def findings(line, canaries):
    """What `line` holds that no captured output may, in a fixed order: an access-key ID and an assignment
    once each, then each canary it holds."""
    found = []
    if ACCESS_KEY.search(line):
        found.append("an access-key ID")
    for match in ASSIGNMENT.finditer(line):
        if holds_value(line, match):
            found.append("an unredacted %s value" % match.group(1).decode("ascii"))
            break
    for index, canary in enumerate(canaries):
        if canary in line:
            found.append("canary %d" % (index + 1))
    return found


class UnreadableText(Exception):
    """A file the patterns cannot be matched against, such as one written as UTF-16."""


def scan(name, stream, canaries):
    """Prints each finding in `stream` as NAME:LINE: WHAT; returns how many lines had one."""
    lines_found = 0
    for number, line in enumerate(stream, start=1):
        if b"\0" in line:
            raise UnreadableText("line %d holds a NUL byte, as UTF-16 text does" % number)
        found = findings(line, canaries)
        for what in found:
            print("%s:%d: %s" % (name, number, what))
        lines_found += 1 if found else 0
    return lines_found


def main():
    parser = argparse.ArgumentParser(description="Fail on a credential or canary in captured output.")
    parser.add_argument("--canary", action="append", default=[], help="a value no output may hold")
    parser.add_argument("files", nargs="+", metavar="FILE")
    arguments = parser.parse_args()
    canaries = [value.encode("utf-8") for value in arguments.canary]
    if any(not canary for canary in canaries):
        parser.error("a canary must not be empty")
    lines_found = 0
    for name in arguments.files:
        try:
            with open(name, "rb") as stream:
                lines_found += scan(name, stream, canaries)
        except OSError as error:
            print("scan-output: cannot read %s: %s" % (name, error.strerror), file=sys.stderr)
            return 2
        except UnreadableText as error:
            print("scan-output: cannot read %s: %s" % (name, error), file=sys.stderr)
            return 2
    return 1 if lines_found else 0


if __name__ == "__main__":
    sys.exit(main())
