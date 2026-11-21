#!/usr/bin/env python3
"""Generate the aws-partition region allowlist from the AWS SDK's published endpoint data.

The region is pasted into the derived host name, where quoting cannot help, so the client wants
an exact list built from published data at a pinned version, never typed by hand.

    gen-region-allowlist.py [--check] [--endpoints PATH] [--list PATH]
    gen-region-allowlist.py --extract PATH

Without --check it rewrites the list; with --check it fails when the list differs from what the data
generates. --extract re-derives the vendored extract from the published file (`-` reads stdin).
README.md in this directory records where the data came from and how to bump the pin.
"""

import argparse
import contextlib
import hashlib
import json
import os
import re
import sys
import tempfile

# The one pin, meant to be the SDK version the driver bundles. README.md, "Which SDK version, and
# why this one", gives the evidence and how to confirm it per platform.
SDK_TAG = "1.11.922"
PUBLISHED_ENDPOINTS_PATH = "tools/code-generation/endpoints/endpoints.json"
PUBLISHED_ENDPOINTS_SHA256 = "ba0ede6b9090b721c9def5535f539d536e40cfba738949ae6ea60218c54b54fc"

# This release supports the aws partition only, so every name in the China, GovCloud,
# isolated and European Sovereign Cloud partitions is left out and refused.
PARTITION = "aws"

# The keys of that partition the extract keeps. Everything else in the published file is dropped:
# the other partitions, this partition's defaults, name and per-service endpoints, and the
# file's version, none of which the allowlist reads.
EXTRACT_KEYS = ("dnsSuffix", "partition", "regionRegex", "regions")

# The extract's record of the published file it was cut from. Generation refuses an extract whose
# record is not the pin, so a pin bumped without re-deriving the extract fails rather than putting
# the new tag on the old names.
PROVENANCE_KEY = "extractedFrom"

# The vendored extract's own SHA-256. Its extractedFrom record can be kept through a hand edit, so
# generation also refuses a vendored extract with any other digest, and an added name shows up in
# review as a change to this line. Bumping the pin sets it after re-deriving the extract.
EXTRACT_SHA256 = "25162e065bf100267744f449ebe93107a9502bee8a8db734016b72cb87fb55a2"

# The suffix the derived host name and the control-plane URL are built from. It is read out of the
# data and checked rather than assumed, so data that gave the aws partition another suffix stops
# this script instead of yielding names those two forms no longer fit.
DNS_SUFFIX = "amazonaws.com"

# A name must match the partition's own `regionRegex`, which refuses pseudo-regions such as
# `aws-global`, and be a plain lower-case token, because that regex's `\w` admits `_` and capitals.
FIRST_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyz")
CHARACTERS = FIRST_CHARACTERS | frozenset("0123456789-")

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRACT_FILE = os.path.join(HERE, "endpoints-aws.json")
LIST_FILE = os.path.join(os.path.dirname(HERE), "region_allowlist.inc")


def fail(message):
    """Print `message` and exit 2, the status every refusal here exits with."""
    print(f"gen-region-allowlist: {message} (see src/transport/odbc/tools/README.md)", file=sys.stderr)
    sys.exit(2)


def read_bytes(path):
    """`path`'s bytes, or standard input's for `-`."""
    if path == "-":
        return sys.stdin.buffer.read()
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError as error:
        fail(f"cannot read {path}: {error}")


def parse(path, data):
    """`data` as JSON, naming `path` when it is not."""
    try:
        return json.loads(data)
    # RecursionError: data nested deeper than the decoder recurses.
    except (ValueError, RecursionError) as error:
        fail(f"{path} does not hold JSON: {error}")


def aws_partition(endpoints):
    """The one `aws` partition in the data, once it carries the suffix the host forms are built from."""
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("partitions", []), list):
        fail("the endpoint data is not an object holding a list of partitions")
    partitions = endpoints.get("partitions", [])
    if not all(isinstance(p, dict) for p in partitions):
        fail("a partition in the endpoint data is not an object")
    named = [p for p in partitions if p.get("partition") == PARTITION]
    if len(named) != 1:
        fail(f"expected exactly one '{PARTITION}' partition in the endpoint data, found {len(named)}")
    partition = named[0]
    if partition.get("dnsSuffix") != DNS_SUFFIX:
        fail(
            f"the '{PARTITION}' partition's dnsSuffix is {partition.get('dnsSuffix')!r}, "
            f"not the '{DNS_SUFFIX}' the host name and the control-plane URL are built from"
        )
    return partition


def region_pattern(partition):
    """The partition's own published region pattern, compiled."""
    pattern = partition.get("regionRegex")
    try:
        return re.compile(pattern, re.ASCII)
    # Beside re.error, a pattern can raise these: TypeError when it is not a string, ValueError for an
    # inline (?u) under re.ASCII, OverflowError for a huge repeat count, and RecursionError when deep.
    except (re.error, TypeError, ValueError, OverflowError, RecursionError):
        fail(f"the '{PARTITION}' partition's regionRegex is not a usable pattern: {pattern!r}")


def aws_regions(partition):
    """The sorted region names of `partition`, each checked to be a plain region token."""
    pattern = region_pattern(partition)
    names = partition.get("regions", {})
    if not isinstance(names, dict):
        fail(f"the '{PARTITION}' partition's regions are not an object keyed by region name")
    regions = sorted(names)
    if not regions:
        fail(f"the '{PARTITION}' partition lists no regions")
    for region in regions:
        if not pattern.fullmatch(region):
            fail(
                f"'{region}' is not matched in full by the '{PARTITION}' partition's own regionRegex "
                f"{pattern.pattern!r}; refusing to allow it into a host name"
            )
        if region[:1] not in FIRST_CHARACTERS or not set(region) <= CHARACTERS:
            fail(f"'{region}' is not a plain lower-case region token; refusing to allow it into a host name")
    return regions


def pinned_provenance():
    """The record --extract writes into the extract and generation requires of it."""
    return {"sdkTag": SDK_TAG, "sha256": PUBLISHED_ENDPOINTS_SHA256}


def check_provenance(path, endpoints):
    """Fail unless the extract at `path` records the pinned published file."""
    if endpoints.get(PROVENANCE_KEY) != pinned_provenance():
        fail(
            f"{path} does not record the pinned published file (aws-sdk-cpp {SDK_TAG}, SHA-256 "
            f"{PUBLISHED_ENDPOINTS_SHA256}) in its {PROVENANCE_KEY} field. Re-derive it with --extract, as "
            "README.md's 'Bumping the pin' says"
        )


def check_vendored_digest(path, data):
    """Fail when `path` is the vendored extract and `data`, its bytes, are not the reviewed ones.

    Another --endpoints file is a test's fixture, which no digest here could name.
    """
    if os.path.realpath(path) != os.path.realpath(EXTRACT_FILE):
        return
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXTRACT_SHA256:
        fail(
            f"{path} has changed since it was reviewed: its SHA-256 is {digest}, not the EXTRACT_SHA256 "
            f"{EXTRACT_SHA256}. Re-derive it with --extract, or after bumping the pin set EXTRACT_SHA256, "
            "as README.md's 'Bumping the pin' says"
        )


def pinned_regions(path):
    """The region names of the extract at `path`.

    The extract must record the pinned published file and, when it is the vendored extract, have
    the reviewed SHA-256.
    """
    data = read_bytes(path)
    check_vendored_digest(path, data)
    endpoints = parse(path, data)
    partition = aws_partition(endpoints)
    check_provenance(path, endpoints)
    return aws_regions(partition)


def extract(path):
    """The aws-partition extract of the SDK's published endpoints.json at `path`."""
    data = read_bytes(path)
    digest = hashlib.sha256(data).hexdigest()
    if digest != PUBLISHED_ENDPOINTS_SHA256:
        fail(
            f"{path} holds data with SHA-256 {digest}, not the {PUBLISHED_ENDPOINTS_SHA256} published at "
            f"aws-sdk-cpp {SDK_TAG}. Fetch {PUBLISHED_ENDPOINTS_PATH} at that tag, or bump the pin as "
            f"README.md's 'Bumping the pin' says"
        )
    return render_extract(parse(path, data))


def render_extract(endpoints):
    """The extract of the published endpoint data `endpoints`, recording the pin.

    Kept out of extract(), which first checks the published file's digest, so that a test can drive
    it with small data that no published file holds.
    """
    partition = aws_partition(endpoints)
    # The same checks the list is generated under, so data this script would refuse to read is
    # never vendored in the first place. They also settle that every kept key is there.
    aws_regions(partition)
    kept = {key: partition[key] for key in EXTRACT_KEYS}
    extract_data = {PROVENANCE_KEY: pinned_provenance(), "partitions": [kept]}
    return json.dumps(extract_data, indent=2, sort_keys=True) + "\n"


def render(regions):
    """The list as a C++ array definition, with where the names came from above it."""
    lines = [
        "// Generated by src/transport/odbc/tools/gen-region-allowlist.py. Do not edit.",
        f"// Source: aws/aws-sdk-cpp tag {SDK_TAG}, {PUBLISHED_ENDPOINTS_PATH},",
        "// through the aws-partition extract in tools/endpoints-aws.json.",
        f"// SHA-256 of the published file: {PUBLISHED_ENDPOINTS_SHA256}",
        f"// Partition: {PARTITION} ({len(regions)} regions, sorted)",
        "//",
        "// The region allowlist: a check on the region before it reaches a derived host name,",
        "// not a list of the regions where a Redshift workgroup or cluster exists, which this",
        "// data does not record.",
        "const char *const AWS_REGIONS[] = {",
    ]
    lines += [f'\t"{region}",' for region in regions]
    lines.append("};")
    return "\n".join(lines) + "\n"


def write_list(path, generated):
    """Write `generated` to the list at `path`, whole or not at all.

    The text goes to a new file beside the list, which then replaces the list, so a write that fails
    part way, on a full disk for example, leaves the old list as it was rather than cut short. The file
    is made new, not opened by a fixed name, so a symlink left at that name is not written through.
    """
    directory, name = os.path.split(path)
    try:
        descriptor, temporary = tempfile.mkstemp(dir=directory or ".", prefix=name + ".")
    except OSError as error:
        fail(f"cannot write {path}: {error}")
    try:
        # mkstemp makes the file 0600; give the list the mode a plain open would have.
        umask = os.umask(0)
        os.umask(umask)
        os.fchmod(descriptor, 0o666 & ~umask)
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(generated)
        os.replace(temporary, path)
    except OSError as error:
        with contextlib.suppress(OSError):
            os.remove(temporary)
        fail(f"cannot write {path}: {error}")
    print(f"gen-region-allowlist: wrote {path} from aws-sdk-cpp {SDK_TAG}")


def check_list(path, generated):
    """Fail unless the list at `path` is `generated`, byte for byte, so rewritten line ends fail too."""
    if read_bytes(path).decode("utf-8", "replace") != generated:
        fail(
            f"{path} is not what aws-sdk-cpp {SDK_TAG}'s endpoint data generates. "
            "Re-run this script without --check, and review the change to the list."
        )
    print(f"gen-region-allowlist: {path} matches aws-sdk-cpp {SDK_TAG}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail when the checked-in list differs")
    mode.add_argument(
        "--extract", metavar="PATH", help="write the extract of the published file at PATH ('-' is stdin)"
    )
    parser.add_argument("--endpoints", default=EXTRACT_FILE, metavar="PATH", help="the endpoint extract to read")
    parser.add_argument(
        "--list", default=LIST_FILE, dest="list_file", metavar="PATH", help="the list to write or check"
    )
    args = parser.parse_args()

    if args.extract is not None:
        sys.stdout.write(extract(args.extract))
        return

    generated = render(pinned_regions(args.endpoints))
    if args.check:
        check_list(args.list_file, generated)
    else:
        write_list(args.list_file, generated)


if __name__ == "__main__":
    main()
