#!/usr/bin/env bash
# Tool tests for embedded_resources.py, which make test_driver's Windows branch runs on each image
# that links the loader. The header is driver_info.py's, from a test lock over a small set; each
# image is written by make_pe.py with that set, or a broken copy of it, as its RCDATA resources.
# Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
odbc="$here/../../../third_party/redshift_odbc"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
sha() { printf '%s' "$1" | python3 -I -c 'import hashlib,sys; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())'; }

# The set: the driver and the three notices, each one short line.
mkdir -p "$tmp/set"
printf 'driver bytes' > "$tmp/set/librsodbc64.so"; printf 'licence' > "$tmp/set/LICENSE"
printf 'notice' > "$tmp/set/NOTICE"; printf 'third party' > "$tmp/set/THIRD_PARTY_LICENSES"
src="https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v2.3.0"
url="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0/AmazonRedshiftODBC-64-bit-2.3.0.0.x86_64.rpm"
printf '%s\n' "format 1" "version 2.3.0.0" "status development" \
  "linux_amd64  driver  librsodbc64.so  $(sha 'driver bytes')  $url  $(printf 'e%.0s' {1..64})  ./opt/x/librsodbc64.so" \
  "*  notice  LICENSE  $(sha licence)  $src/LICENSE" "*  notice  NOTICE  $(sha notice)  $src/NOTICE" \
  "*  notice  THIRD_PARTY_LICENSES  $(sha 'third party')  $src/THIRD_PARTY_LICENSES" > "$tmp/test.lock"
bash "$odbc/tools/check-lock.sh" "$tmp/test.lock" | python3 -I "$odbc/tools/lib/driver_info.py" linux_amd64 "$tmp/set" "$odbc/policy"
header="$tmp/set/driver_info.h"

# check <name> <expected-exit> <expected-substring> <json resources>: one image with those
# resources, as RCDATA (type 10) in language 0 unless given.
check() {
  local name="$1" want="$2" needle="$3" out status
  python3 -I "$here/make_pe.py" "$tmp/image.dll" "{\"resources\": $4, \"trailing\": \"metadata\"}"
  out="$(python3 -I "$here/embedded_resources.py" "$header" "$tmp/image.dll" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then pass "$name"
  else fail "$name (exit $status, wanted $want; output: $out)"; fi
}
set='[10, "DRIVER", 0, "driver bytes"], [10, "NOTICE_LICENSE", 0, "licence"], [10, "NOTICE_NOTICE", 0, "notice"],
  [10, "NOTICE_THIRD_PARTY_LICENSES", 0, "third party"]'
check "passes an image whose RCDATA resources are the set, beside a version resource" 0 \
  "holds the 4 files of the set, as driver_info.h records them" "[$set, [16, 1, 1033, \"version\"]]"
check "fails a resource whose bytes differ from the header's, naming both hashes" 1 \
  "RCDATA \"NOTICE_NOTICE\" (NOTICE) is 6 bytes with SHA-256 $(sha notixe); driver_info.h records 6 bytes with $(sha notice)" \
  "[${set/\"notice\"/\"notixe\"}]"
check "fails a resource one byte longer than the header's" 1 "RCDATA \"DRIVER\" (librsodbc64.so) is 13 bytes" \
  "[${set/driver bytes/driver bytes!}]"
check "fails a file of the set that is missing" 1 "RCDATA \"NOTICE_LICENSE\" (LICENSE) is there 0 times, not once" \
  "[${set/\[10, \"NOTICE_LICENSE\", 0, \"licence\"\], /}]"
check "fails a file of the set in a second language" 1 "RCDATA \"DRIVER\" (librsodbc64.so) is there 2 times" \
  "[$set, [10, \"DRIVER\", 1033, \"driver bytes\"]]"
check "fails an RCDATA resource that is not a file of the set" 1 "RCDATA \"EXTRA\" is not a file of the set" \
  "[$set, [10, \"EXTRA\", 0, \"x\"]]"
check "fails an image with no resources, naming each file" 1 "RCDATA \"NOTICE_THIRD_PARTY_LICENSES\" (THIRD_PARTY_LICENSES) is there 0 times" '[]'

# Every image is checked, and one that fails fails the run.
python3 -I "$here/make_pe.py" "$tmp/good.dll" "{\"resources\": [$set]}"
python3 -I "$here/make_pe.py" "$tmp/bad.dll" '{"resources": [[10, "DRIVER", 0, "x"]]}'
out="$(python3 -I "$here/embedded_resources.py" "$header" "$tmp/good.dll" "$tmp/bad.dll" "$tmp/good.dll" 2>&1)" &&
  status=0 || status=$?
if [[ "$status" == 1 && "$(grep -c 'holds the 4 files' <<<"$out")" == 2 && "$out" == *"$tmp/bad.dll: RCDATA"* ]]; then
  pass "checks every image it is given, and fails when one fails"
else fail "several images (exit $status; $out)"; fi
printf 'not a PE' > "$tmp/text.dll"
out="$(python3 -I "$here/embedded_resources.py" "$header" "$tmp/text.dll" "$tmp/missing.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == *"text.dll: image: not a PE image"* && "$out" == *"missing.dll: cannot read it"* ]]; then
  pass "fails a file that is not a PE image, and one that is missing"
else fail "not an image, or missing (exit $status; $out)"; fi
grep -v '^#define REDSHIFT_DRIVER_FILES' "$header" > "$tmp/no-list.h"
out="$(python3 -I "$here/embedded_resources.py" "$tmp/no-list.h" "$tmp/good.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == *"has no complete REDSHIFT_DRIVER_FILES list"* ]]; then
  pass "fails a header without the file list"
else fail "a header without the file list (exit $status; $out)"; fi
# A file list cut short, and one with a row of another form, are not complete lists either. BSD sed
# reads no \t, so the tab is a literal.
tab=$'\t'
sed "/^${tab}X(\"THIRD_PARTY_LICENSES\"/d" "$header" > "$tmp/cut.h"
sed "s/^${tab}X(\"NOTICE\", 6,/${tab}X(NOTICE, 6,/" "$header" > "$tmp/unquoted.h"
cut="$(python3 -I "$here/embedded_resources.py" "$tmp/cut.h" "$tmp/good.dll" 2>&1)" && cut_status=0 || cut_status=$?
out="$(python3 -I "$here/embedded_resources.py" "$tmp/unquoted.h" "$tmp/good.dll" 2>&1)" && status=0 || status=$?
if [[ "$cut_status" == 1 && "$cut" == *"has no complete REDSHIFT_DRIVER_FILES list"* && "$status" == 1 &&
  "$out" == *"has no complete REDSHIFT_DRIVER_FILES list"* ]]; then
  pass "fails a file list cut short, and one with a row of another form"
else fail "a file list cut short or malformed ($cut_status $cut; $status $out)"; fi
printf '#define X "\303\251"\n' > "$tmp/utf8.h"
out="$(python3 -I "$here/embedded_resources.py" "$tmp/utf8.h" "$tmp/no-image.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "embedded_resources: cannot read $tmp/utf8.h: "*"codec can't decode"* ]]; then
  pass "a header that is not ASCII is refused as unreadable, not with a traceback"
else fail "a header that is not ASCII (exit $status; $out)"; fi
out="$(python3 -I "$here/embedded_resources.py" "$header" 2>&1)" && status=0 || status=$?
if [[ "$status" == 2 && "$out" == "usage: embedded_resources.py <driver_info.h> <image>..." ]]; then
  pass "prints its usage without an image"
else fail "usage (exit $status; $out)"; fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
