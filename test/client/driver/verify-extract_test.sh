#!/usr/bin/env bash
# Tool tests for verification's extraction of the driver and of the notices,
# third_party/redshift_odbc/tools/lib/verify_linux.sh: the member checks' fixtures (make_cpio.py),
# wrapped in packages signed by a test PKI made per run, each fail at the extraction, print no row
# and leave nothing in the working or temporary directory; the notices come from the release tag
# and must equal the checked-in copies, and a package that holds a member named like one fails the
# run; the RPM reader refuses every header it cannot read as one string per field and every payload
# it does not accept. Run by run-tool-tests.sh.
#
# A test package is a minimal RPM (make_rpm.py) holding the newc archive gzip-compressed, as a
# driver package does, which third_party/redshift_odbc/tools/lib/rpm.py reads; the real packages
# are read the same way in verify-release_test.sh. The driver member is the real driver of the
# lock under test, from the release cache (release-cache.sh).
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=verify-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/verify-test-lib.sh"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

base_lock="${RS_DRIVER_LOCK:-$odbc/driver.lock}"
[[ "$base_lock" == /* ]] || base_lock="$PWD/$base_lock"
# The suite runs on the first Linux platform the lock names, P with architecture A. A lock that
# check-lock.sh rejects fails the suite here, with its message; only a valid lock without a Linux
# row skips.
lock="$(bash "$tools/check-lock.sh" "$base_lock")"
read -r _ P _ _ _ url pkg_sha driver_path <<<"$(awk '$1 == "driver" && $2 ~ /^linux_/' <<<"$lock" | head -1)"
case "${P:-}" in
linux_amd64) A=x86_64 ;;
linux_arm64) A=aarch64 ;;
*) echo "1..0 # SKIP the lock under test names no Linux platform"; exit 0 ;;
esac
cached="$(release_file "$pkg_sha" "$url" "$pkg_sha")"
python3 -I "$tools/lib/rpm.py" --payload "$cached" | python3 -I "$tools/lib/cpio.py" "$driver_path" "$tmp/driver"

t="$(test_tree tree)"
version=2.3.0.0
serve="$tmp/serve"; mkdir -p "$serve" "$tmp/scratch" "$tmp/cwd"
export FAKE_CURL_DIR="$serve"
# release <member>...: a test-signed package of $version holding those members (make_cpio.py's
# syntax), and the release tag's notices equal to the checked-in ones, all served by the stand-in
# curl. Sets package and tag.
# RPM_NAME, when set, is the name the package's header records instead of the driver package's.
release() {
  package="AmazonRedshiftODBC-64-bit-$version.$A" tag="https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}"
  rm -f "$serve"/*
  python3 -I "$here/make_cpio.py" "$tmp/payload" "$@"
  python3 -I "$here/make_rpm.py" "$serve/$package.rpm" "${RPM_NAME:-AmazonRedshiftODBC-64-bit}" "${version%.*}" "${version##*.}" "$A" \
    < "$tmp/payload"
  pki_sign leaf "$serve/$package.rpm" "$serve/$package-signature.bin"
  cp "$pki/leaf.pem" "$serve/$package-certificate.pem"
  cp "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "$serve/"
}
driver="$driver_path:file:1:$tmp/driver"
# run [<tree>]: verifies the served release against the test PKI, in an empty working directory
# and with an empty TMPDIR; sets out_text and status, and clean when nothing was left anywhere.
run() {
  rm -f "$FAKE_CURL_LOG"
  out_text="$(cd "$tmp/cwd" && env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" TMPDIR="$tmp/scratch" ${env_extra[@]+"${env_extra[@]}"} \
    bash "$tools/lib/verify_linux.sh" "$P" "$version" "${1:-$t}" "$repo/src/transport/odbc_driver.hpp" "${BUNDLE-$pki/bundle.pem}" 2>&1)" && status=0 || status=$?
  # Nothing may be left in the working directory or TMPDIR, and no member named escape or abs
  # anywhere under the test's directory.
  clean=1
  [[ -z "$(ls -A "$tmp/cwd")$(ls -A "$tmp/scratch")" && -z "$(find "$tmp" -name escape -o -name abs)" ]] || clean=0
}
env_extra=()
# fails <name> <rule substring>: the run stopped with that message, as its last line, before any
# ELF rule ran, printed no lock row and left nothing behind.
fails() {
  if [[ "$status" == 1 && "${out_text##*$'\n'}" == *"$2"* && "$out_text" != *" driver "* &&
    "$out_text" != *"Linux policy"* && "$clean" == 1 ]]; then pass "$1"; else
    fail "$1 (exit $status, clean $clean; output: $out_text)"; fi
}
row() { awk -v n="$1" '$2 == "notice" && $3 == n { print $5 } $2 == "driver" && n == "driver" { print $7 }' <<<"$out_text"; }

release "$driver" ./opt/amazon/redshiftodbcx64/odbc.ini:file ./usr/lib/x:link; run
if [[ "$status" == 0 && "$clean" == 1 && "$(row driver)" == "$driver_path" && "$(row LICENSE)" == "$tag/LICENSE" &&
  "$out_text" == *"$P  driver  librsodbc64.so  $(sha "$tmp/driver")  "* ]]; then
  pass "a test-signed package passes, and the driver row has the member's hash and the policy's path"
else fail "test-signed package (exit $status; $out_text)"; fi
if [[ "$(row NOTICE)" == "$tag/NOTICE" && "$(row THIRD_PARTY_LICENSES)" == "$tag/THIRD_PARTY_LICENSES" ]] &&
  grep -qx "$tag/THIRD_PARTY_LICENSES" "$FAKE_CURL_LOG"; then
  pass "every notice comes from the release tag, which the rows record"; else fail "notice sources: $out_text"; fi

# The stand-in curl's argument log holds the last run's calls only, so it never grows into
# download.sh's size bound however many runs the suite makes or however long TMPDIR is.
run
if [[ "$status" == 0 && "$(grep -c '^@@$' "$FAKE_CURL_LOG.args")" == "$(($(wc -l < "$FAKE_CURL_LOG")))" ]]; then # BSD wc pads its count
  pass "a second run's curl argument log holds that run's calls only"
else fail "curl argument log: $(grep -c '^@@$' "$FAKE_CURL_LOG.args") calls logged, $(($(wc -l < "$FAKE_CURL_LOG"))) made"; fi

# The driver's member: the member checks' fixtures, each failing at the extraction.
for c in "link:a symbolic link" "dir:a directory" "fifo:a FIFO" "dev:a device node"; do
  release "$driver_path:${c%%:*}"; run
  fails "${c#*:} at the driver's path fails at the extraction" "extract: cannot take $driver_path from the package: cpio.py: the member at $driver_path is not a regular file"
done
release "$driver_path:file:2:$tmp/driver"; run
fails "a hard-linked driver fails at the extraction" "has 2 links, not one"
release "$driver" "$driver"; run
fails "two members at the driver's path fail" "more than one member at $driver_path"
release ../escape:file "$driver"; run
fails "a ../escape member before the driver fails at the extraction" "member b'../escape' has an absolute path or a .. segment"
release "$driver" /abs:file; run
fails "an /abs member after the driver fails at the extraction" "member b'/abs' has an absolute path"
release ./opt/other.so:file; run
fails "a package without the driver's path fails" "no member at $driver_path"
release '!badhex' "$driver"; run
fails "a malformed archive fails" "not hexadecimal"
# Signed bytes that are not an RPM package: the archive itself, signed by the test leaf.
release "$driver"; mv "$tmp/payload" "$serve/$package.rpm"; pki_sign leaf "$serve/$package.rpm" "$serve/$package-signature.bin"; run
fails "signed bytes whose RPM header cannot be read fail" "package: the signed package's RPM header cannot be read: not an RPM package"
RPM_NAME=AmazonRedshiftODBC-32-bit release "$driver"; run
fails "a signed package of another name fails" \
  "package: the signed package is AmazonRedshiftODBC-32-bit ${version%.*} ${version##*.} $A (name version release architecture), not the requested AmazonRedshiftODBC-64-bit"
nopath="$tmp/no-path/third_party/redshift_odbc"; mkdir -p "$nopath"; cp -R "$t/." "$nopath/"; rm "$nopath/policy/linux/package-path.txt"
release "$driver"; env_extra=(FAKE_CURL=fail); run "$nopath"; env_extra=()
if [[ "$status" == 1 && "${out_text##*$'\n'}" == *"policy: policy/linux/package-path.txt is missing"* && ! -e "$FAKE_CURL_LOG" ]]; then
  pass "a tree without package-path.txt fails at the policy, before any download"; else fail "no package-path.txt (exit $status; $out_text)"; fi
# No trust store (verify-release.sh passes an empty path when it finds no system bundle): the run
# fails at the chain before any download.
for missing_bundle in "$tmp/no-such-bundle.pem" ""; do
  release "$driver"; env_extra=(FAKE_CURL=fail); BUNDLE="$missing_bundle" run; env_extra=()
  if [[ "$status" == 1 && "${out_text##*$'\n'}" == *"trust-store: no system CA bundle"* && ! -e "$FAKE_CURL_LOG" ]]; then
    what="no trust store found"; [[ -z "$missing_bundle" ]] || what="a trust store missing at its path"
    pass "$what fails at the trust store, before any download"
  else fail "no trust store '$missing_bundle' (exit $status; $out_text)"; fi
done

# The policy's path, checked before any download.
for bad in ./opt/../x /opt/x $'./opt/x\n./opt/y'; do
  t2="$(test_tree "bad-path-${#bad}")"; printf '%s\n' "$bad" > "$t2/policy/linux/package-path.txt"
  env_extra=(FAKE_CURL=fail); run "$t2"; env_extra=()
  if [[ "$status" == 1 && "$out_text" == *"policy: policy/linux/package-path.txt"* && ! -e "$FAKE_CURL_LOG" ]]; then
    pass "a package path '${bad//$'\n'/' and '}' fails before any download"; else fail "package path (exit $status; $out_text)"; fi
done
# Its last segment must be a file name the lock accepts for the driver.
for bad in ./opt/+x.so ./opt/packages ./opt/x/LICENSE ./opt/x/notice ./opt/.x.so; do
  t2="$(test_tree "bad-name-${bad//[\/.+]/_}")"; printf '%s\n' "$bad" > "$t2/policy/linux/package-path.txt"
  env_extra=(FAKE_CURL=fail); run "$t2"; env_extra=()
  if [[ "$status" == 1 && "$out_text" == *"policy: policy/linux/package-path.txt: check-lock: '${bad##*/}': file name"* && ! -e "$FAKE_CURL_LOG" ]]; then
    pass "a driver file name '${bad##*/}' fails before any download"; else fail "driver name $bad (exit $status; $out_text)"; fi
done

# The notices from the release tag.
release "$driver"; printf 'changed\n' > "$serve/THIRD_PARTY_LICENSES"; run
fails "a changed THIRD_PARTY_LICENSES at the release tag fails, naming it and both hashes" \
  "notices: THIRD_PARTY_LICENSES from $tag/THIRD_PARTY_LICENSES has SHA-256 $(sha "$serve/THIRD_PARTY_LICENSES"); the checked-in copy has $(sha "$odbc/THIRD_PARTY_LICENSES")"
release "$driver"; rm "$serve/NOTICE"; run
fails "a notice missing at the release tag fails" "download: downloading $tag/NOTICE failed"
release "$driver"; head -c 2000000 /dev/zero > "$serve/NOTICE"; run
fails "a notice over 1 MiB at the release tag is stopped while it is written" "download: downloading $tag/NOTICE failed"
t2="$(test_tree no-notice)"; rm "$t2/LICENSE"
release "$driver"; run "$t2"
fails "a missing checked-in notice fails" "notices: the checked-in LICENSE is missing"

# The release tag's copy ships. A package may hold each notice once beside the driver, as a regular
# file with the tag's bytes (2.3.1.0's packages do); its copy is checked, and the rows still name the
# tag. Anything else named like a notice fails the run, naming every such member.
d=./opt/amazon/redshiftodbcx64
release "$driver" "$d/LICENSE:file:1:$odbc/LICENSE" "$d/NOTICE:file:1:$odbc/NOTICE" \
  "$d/THIRD_PARTY_LICENSES:file:1:$odbc/THIRD_PARTY_LICENSES"; run
if [[ "$status" == 0 && "$clean" == 1 && "$(row LICENSE)" == "$tag/LICENSE" && "$(row THIRD_PARTY_LICENSES)" == "$tag/THIRD_PARTY_LICENSES" ]]; then
  pass "a package holding the release tag's notices beside the driver passes, and the rows name the tag"
else fail "a package with the tag's notices beside the driver (exit $status; $out_text)"; fi
printf 'another notice text\n' > "$tmp/other-notice"
for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
  release "$driver" "$d/$notice:file:1:$tmp/other-notice"; run
  fails "a $notice beside the driver with other bytes than the release tag's fails, naming both" \
    "notices: the package's $d/$notice has SHA-256 $(sha "$tmp/other-notice"); $notice from $tag/$notice has $(sha "$odbc/$notice")"
done
printf 'placeholder-target' > "$tmp/target"
for c in "link:1:$tmp/target:a symbolic link" "fifo:1:$tmp/target:a FIFO" "dev:1:$tmp/target:a device node" \
  "dir:1:$tmp/target:a directory" "file:2:$odbc/LICENSE:a file with two links"; do
  IFS=: read -r kind links data what <<<"$c"
  release "$driver" "$d/LICENSE:$kind:$links:$data"; run
  fails "$what at a notice's place beside the driver fails, naming it" "notices: cannot take $d/LICENSE from the package"
done
release "$driver" "$d/LICENSE:file:1:$odbc/LICENSE" "./usr/share/doc/NOTICE:file:1:$odbc/NOTICE"; run
fails "of two members named like notices, only the one out of place is named" "notices: the package holds ./usr/share/doc/NOTICE, named like a notice but not the one regular copy beside the driver; the release tag's copy ships"
release "$driver" "$d/LICENSE:file:1:$odbc/LICENSE" "$d/LICENSE:file:1:$odbc/LICENSE"; run
fails "two members at one notice's path fail, the second named" "notices: the package holds $d/LICENSE, named like a notice but not the one regular copy beside the driver; the release tag's copy ships"
release "$driver" "$d/NOTICE:file:1:$odbc/NOTICE" ../escape:file; run
fails "a ../escape member beside a notice fails while the members are read" "member b'../escape' has an absolute path or a .. segment"
release "$driver" "./a b/LICENSE:file:1:$odbc/LICENSE"; run
fails "a member named like a notice whose path has a space fails" "notices: cannot read the package's members: cpio.py: member b'./a b/LICENSE' has a space"
# The notice names are matched ignoring case, as the lock's are, so a case-insensitive file system
# cannot let a package's own copy through.
release "$driver" "$d/License:file:1:$odbc/LICENSE"; run
fails "a member named like a notice in another case fails" "notices: the package holds $d/License, named like a notice but not the one regular copy beside the driver; the release tag's copy ships"

# The release tag is the only source, and its rule covers <a>.<b>.<c>.0 alone.
version=2.3.0.1
release "$driver"; env_extra=(FAKE_CURL=fail); run; env_extra=()
fails "a version that is not <a>.<b>.<c>.0 has no release-tag rule" \
  "notices: there is no release-tag rule for 2.3.0.1"
[[ ! -e "$FAKE_CURL_LOG" ]] || fail "the release-tag rule ran after a download: $(tr '\n' ' ' < "$FAKE_CURL_LOG")"
version=2.2.4.0
release "$driver"; run
if [[ "$status" == 0 && "$(row LICENSE)" == "https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v2.2.4/LICENSE" &&
  "$out_text" == *"$P  driver  librsodbc64.so  $(sha "$tmp/driver")  https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.2.4.0/"* ]]; then
  pass "a release below 2.3.0.0 verifies too and prints its rows, notices from its tag"; else fail "2.2.4.0 (exit $status; $out_text)"; fi
version=2.3.0.0

# The RPM reader (third_party/redshift_odbc/tools/lib/rpm.py) on headers built here: it reads one
# string each for the name, version, release and architecture, and refuses anything else.
# make_rpm.py writes well-formed packages; this builder exists for the headers it must not be able
# to write.
# header <out> <entries>: a package with the test lead, an empty signature header and a header of
# <entries>, a Python list of (tag, type, value) with value a str; MAGIC=bad, COUNT=<n>, TRUNCATE=1
# break the header, and ENTRY_COUNT=<n> and UNTERMINATED=1 the last entry (its value count, its
# NUL).
header() {
  python3 -I - "$1" "$2" <<'PY'
import os, struct, sys
entries = eval(sys.argv[2])
lead = b"\xed\xab\xee\xdb" + b"\x00" * 92
def structure(entries, magic=b"\x8e\xad\xe8\x01", count=None):
    index, store = b"", b""
    for i, (tag, kind, value) in enumerate(entries):
        last = i == len(entries) - 1
        index += struct.pack(">IIII", tag, kind, len(store), int(os.environ.get("ENTRY_COUNT", 1)) if last else 1)
        store += value.encode() + (b"" if last and os.environ.get("UNTERMINATED") else b"\x00")
    n = len(entries) if count is None else count
    return magic + b"\x00" * 4 + struct.pack(">II", n, len(store)) + index + store
main = structure(entries, b"\x00\x00\x00\x00" if os.environ.get("MAGIC") == "bad" else b"\x8e\xad\xe8\x01",
                 int(os.environ["COUNT"]) if os.environ.get("COUNT") else None)
data = lead + structure([]) + main
if os.environ.get("TRUNCATE"):
    data = data[:-3]
open(sys.argv[1], "wb").write(data)
PY
}
good='(1000, 6, "AmazonRedshiftODBC-64-bit"), (1001, 6, "2.3.0"), (1002, 6, "0"), (1022, 6, "x86_64")'
# rpm_case <name> <expected-exit> <expected-output> <entries> [<env>...]
rpm_case() {
  local name="$1" want="$2" needle="$3" entries="$4" out status
  shift 4
  ( if (($#)); then export "$@"; fi; header "$tmp/h.rpm" "[$entries]" )
  out="$(python3 -I "$tools/lib/rpm.py" "$tmp/h.rpm" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then pass "rpm.py: $name"; else
    fail "rpm.py: $name (exit $status, wanted $want; output: $out)"; fi
}
rpm_case "a well-formed header gives its name, version, release and architecture" 0 "AmazonRedshiftODBC-64-bit 2.3.0 0 x86_64" "$good"
printf 'not an rpm at all, but long enough to have a lead of ninety-six bytes and more after it..........' > "$tmp/h.rpm"
out_text="$(python3 -I "$tools/lib/rpm.py" "$tmp/h.rpm" 2>&1)" && status=0 || status=$?
expect "rpm.py: a file without the RPM lead fails" 1 "not an RPM package"
rpm_case "a header without its magic fails" 1 "no RPM header at offset" "$good" MAGIC=bad
rpm_case "a header with more entries than it accepts fails" 1 "larger than this reader accepts" "$good" COUNT=70000
# The 64 MiB bound on the header's string store, reached with one value of 64 MiB and one byte
# (the entries are a Python expression, so the fixture never passes those bytes as an argument).
rpm_case "a header whose string store is over 64 MiB fails" 1 "larger than this reader accepts" \
  '(1000, 6, "x" * ((64 << 20) + 1)), (1001, 6, "2.3.0"), (1002, 6, "0"), (1022, 6, "x86_64")' 
rpm_case "a package that ends inside its header fails" 1 "the package ends inside its header" "$good" TRUNCATE=1
rpm_case "a tag recorded twice fails" 1 "the header records tag 1000 twice" "$good, (1000, 6, \"Other\")"
rpm_case "a tag that is not one string fails" 1 "the header's tag 1022 is not one string" '(1000, 6, "AmazonRedshiftODBC-64-bit"), (1001, 6, "2.3.0"), (1002, 6, "0"), (1022, 4, "x86_64")'
rpm_case "a tag with two values fails" 1 "the header's tag 1022 is not one string" "$good" ENTRY_COUNT=2
rpm_case "a value without its terminating NUL fails" 1 "the header's tag 1022 is not one string" "$good" UNTERMINATED=1
rpm_case "a value with a space fails" 1 "the header's tag 1001 is empty or holds a byte that is not printable ASCII" '(1000, 6, "AmazonRedshiftODBC-64-bit"), (1001, 6, "2.3.0 x"), (1002, 6, "0"), (1022, 6, "x86_64")'
# A value the package controls reaches a message, so a byte that is not printable ASCII (here an
# escape, which would colour or retitle a terminal) fails rather than being printed.
rpm_case "a value with a control character fails" 1 "the header's tag 1000 is empty or holds a byte that is not printable ASCII" \
  '(1000, 6, "Amazon\x1b[31mRED"), (1001, 6, "2.3.0"), (1002, 6, "0"), (1022, 6, "x86_64")'
rpm_case "an empty value fails" 1 "the header's tag 1002 is empty" '(1000, 6, "AmazonRedshiftODBC-64-bit"), (1001, 6, "2.3.0"), (1002, 6, ""), (1022, 6, "x86_64")'
rpm_case "a header without the architecture fails" 1 "the header records no arch" '(1000, 6, "AmazonRedshiftODBC-64-bit"), (1001, 6, "2.3.0"), (1002, 6, "0")'

# The payload rpm.py accepts: cpio compressed with gzip, the form every driver package has, and
# nothing else. payload_case <name> <substring> <env>...
payload_case() {
  local name="$1" needle="$2" out status
  shift 2
  python3 -I "$here/make_cpio.py" "$tmp/payload" "$driver"
  ( export "$@"; python3 -I "$here/make_rpm.py" "$tmp/p.rpm" AmazonRedshiftODBC-64-bit 2.3.0 0 "$A" < "$tmp/payload" )
  out="$(python3 -I "$tools/lib/rpm.py" --payload "$tmp/p.rpm" 2>&1 >/dev/null)" && status=0 || status=$?
  if [[ "$status" == 1 && "$out" == *"$needle"* ]]; then pass "rpm.py: $name"; else
    fail "rpm.py: $name (exit $status, wanted 1; output: $out)"; fi
}
payload_case "a payload declared xz is refused" "compressed with xz; this reader takes cpio compressed with gzip" PAYLOAD_COMPRESSOR=xz
payload_case "a payload declared as another format is refused" "the payload is tar compressed with gzip" PAYLOAD_FORMAT=tar
payload_case "a payload that is not the gzip stream it declares fails" "" PAYLOAD_RAW=1
python3 -I "$here/make_cpio.py" "$tmp/payload" "$driver"
python3 -I "$here/make_rpm.py" "$tmp/p.rpm" AmazonRedshiftODBC-64-bit 2.3.0 0 "$A" < "$tmp/payload"
printf 'trailing\n' >> "$tmp/p.rpm"
out_text="$(python3 -I "$tools/lib/rpm.py" --payload "$tmp/p.rpm" 2>&1 >/dev/null)" && status=0 || status=$?
expect "rpm.py: data after the gzip payload fails" 1 "data follows the package's gzip payload"
python3 -I "$here/make_rpm.py" "$tmp/p.rpm" AmazonRedshiftODBC-64-bit 2.3.0 0 "$A" < "$tmp/payload"
python3 -I -c 'import sys
data = open(sys.argv[1], "rb").read()
open(sys.argv[1], "wb").write(data[:-20])' "$tmp/p.rpm"
out_text="$(python3 -I "$tools/lib/rpm.py" --payload "$tmp/p.rpm" 2>&1 >/dev/null)" && status=0 || status=$?
expect "rpm.py: a package that ends inside its payload fails" 1 "the package ends inside its gzip payload"
# The payload rpm.py writes is the archive it was given, byte for byte.
python3 -I "$here/make_rpm.py" "$tmp/p.rpm" AmazonRedshiftODBC-64-bit 2.3.0 0 "$A" < "$tmp/payload"
if [[ "$(python3 -I "$tools/lib/rpm.py" --payload "$tmp/p.rpm" | sha "/dev/stdin")" == "$(sha "$tmp/payload")" ]]; then
  pass "rpm.py: the payload it writes is the archive the package holds"; else fail "payload bytes"; fi

# Python's hashlib reads OPENSSL_CONF too, and verification runs without it.
if hostile_case "with an OPENSSL_CONF that openssl applies, a test-signed package still passes"; then
  release "$driver"; env_extra=(OPENSSL_CONF="$hostile_conf"); run; env_extra=()
  if [[ "$status" == 0 && "$(row driver)" == "$driver_path" ]]; then
    pass "with an OPENSSL_CONF that openssl applies, a test-signed package still passes"; else fail "hostile OPENSSL_CONF (exit $status; $out_text)"; fi
fi
finish
