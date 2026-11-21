#!/usr/bin/env bash
# Tool tests for the macOS library's slices: lib/macho.py's slices rule, which every
# macho.py command applies to the universal librsodbc64.dylib before it reads a slice, and
# macho.py thin, which writes the two slices out for the environment-name rule. The library's own
# code signature is not checked: the package signature authenticates it. The
# unmodified locked library passes; copies of it with one field changed by patch_macho.py, and tiny
# libraries make_macho.py writes, each fail at their rule. Run by run-tool-tests.sh.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=macos-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/macos-test-lib.sh"

# check <dylib>: the slices rule, through macho.py thin into a new directory.
check() {
  rm -rf -- "${tmp:?}/thin"; mkdir "$tmp/thin"
  out_text="$(python3 -I "$lib/macho.py" thin "$1" "$tmp/thin" 2>&1)" && status=0 || status=$?
}
patch() { python3 -I "$here/patch_macho.py" "$@"; }
# same_slices <dylib>: the files thin wrote are the universal header's two slices, byte for byte,
# read here from the header rather than by the tool under test.
same_slices() {
  python3 -I -c 'import struct, sys
data = open(sys.argv[1], "rb").read()
names = {0x01000007: "x86_64", 0x0100000C: "arm64"}
for i in range(struct.unpack(">I", data[4:8])[0]):
    cputype, _, offset, size, _ = struct.unpack(">IIIII", data[8 + 20 * i:28 + 20 * i])
    if open("%s/%s" % (sys.argv[2], names[cputype]), "rb").read() != data[offset:offset + size]:
        sys.exit(1)' "$1" "$tmp/thin"
}

real="$(real_package)"
if [[ -n "$real" ]]; then
  python3 -I "$lib/xar.py" extract "$real" "$mac_path" "$tmp/real.dylib" sha1
  r="$tmp/real.dylib" p="$tmp/p.dylib"
  check "$r"
  if [[ "$status" == 0 && -z "$out_text" ]] && same_slices "$r"; then
    pass "the unmodified locked library's two slices pass, and thin writes each byte for byte"
  else fail "the locked library's slices (exit $status; $out_text)"; fi
  [[ "$(sha "$r")" == "$mac_sha" ]] && pass "the extracted library is the lock's" || fail "the extracted library is not the lock's"
  patch "$r" "$p" drop arm64; check "$p"
  expect "a removed slice fails" 1 "slices: the library has 1 slices; it must have exactly x86_64 and arm64"
  patch "$r" "$p" append; check "$p"
  expect "bytes after the last slice fail" 1 "slices: the library holds bytes after its last slice"
  patch "$r" "$p" subtype arm64 2; check "$p"
  expect "an arm64e slice fails" 1 "slices: the arm64 slice is the subtype 2, not the whole architecture"
  patch "$r" "$p" size arm64 0x7fffffff; check "$p"
  expect "a slice past the end of the file fails" 1 "slices: the arm64 slice lies outside the file or is not aligned"
  patch "$r" "$p" offset arm64 0x8000; check "$p"
  expect "overlapping slices fail" 1 "slices: the library's slices overlap"
  patch "$r" "$p" swap; check "$p"
  expect "a slice whose header names another architecture fails" 1 "slices: the x86_64 slice's header names another architecture"
  patch "$r" "$p" filetype x86_64 2; check "$p"
  expect "a slice that is not a dynamic library fails" 1 "slices: the x86_64 slice is not a 64-bit Mach-O dynamic library"
  patch "$r" "$p" sizeofcmds arm64 8; check "$p"
  expect "load commands that do not fill their size fail" 1 "slices: the arm64 slice's load commands do not fill the size its header gives"
  [[ -z "$(ls -A "$tmp/thin")" ]] && pass "thin writes nothing for a library that fails the slices rule" ||
    fail "thin wrote $(ls "$tmp/thin") for a failing library"
else skip "the locked library's slices" "$no_real"; fi

# Every rule again on tiny libraries, so a default run fails without the real package.
t="$tmp/t.dylib" tp="$tmp/tp.dylib"
python3 -I "$here/make_macho.py" "$t" '{"exports": ["SQLAllocHandle"]}'
# tcase <name> <substring> <change> [<arch> [<argument>]]: the changed tiny library.
tcase() {
  local name="$1" needle="$2"; shift 2
  patch "$t" "$tp" "$@"; check "$tp"
  expect "$name" 1 "$needle"
}
# tlib <name> <substring> <json spec>: a tiny library make_macho.py writes.
tlib() { python3 -I "$here/make_macho.py" "$tp" "$3"; check "$tp"; expect "$1" 1 "$2"; }
check "$t"
if [[ "$status" == 0 ]] && same_slices "$t"; then pass "a tiny library's slices pass, and thin writes each byte for byte"
else fail "a tiny library's slices (exit $status; $out_text)"; fi
tlib "two x86_64 slices fail" "slices: the library's slices are not exactly x86_64 and arm64" '{"archs": ["x86_64", "x86_64"]}'
tlib "one slice fails" "slices: the library has 1 slices" '{"archs": ["x86_64"]}'
tlib "three slices fail" "slices: the library has 3 slices" '{"archs": ["x86_64", "arm64", "arm64"]}'
patch "$t" "$tp" drop arm64; check "$tp"
[[ "$status" == 1 && "$out_text" == "slices: the library has 1 slices; it must have exactly x86_64 and arm64" ]] &&
  pass "a tiny library without its arm64 slice fails, in one line" || fail "a dropped slice (exit $status; $out_text)"
tcase "a byte after the last slice fails" "slices: the library holds bytes after its last slice" append
tcase "a byte between the slices fails" "slices: the library holds bytes other than zero between its slices" gap
tcase "a byte between the universal header and the first slice fails" "slices: the library holds bytes other than zero between its slices" gap-header
tcase "an arm64e fat entry fails" "slices: the arm64 slice is the subtype 2, not the whole architecture" subtype arm64 2
tcase "an x86_64h fat entry fails" "slices: the x86_64 slice is the subtype 8, not the whole architecture" subtype x86_64 8
tcase "an arm64e slice header under an arm64 fat entry fails" \
  "slices: the arm64 slice's header is the subtype 2, not the whole architecture" header-subtype arm64 0x80000002
tcase "an x86_64h slice header under an x86_64 fat entry fails" \
  "slices: the x86_64 slice's header is the subtype 8, not the whole architecture" header-subtype x86_64 8
patch "$t" "$tp" header-subtype x86_64 0x80000003; check "$tp"
[[ "$status" == 0 ]] && pass "the high byte of a slice header's subtype holds capability bits, which pass" ||
  fail "capability bits (exit $status; $out_text)"
tcase "an unknown CPU type fails" "slices: the library's slices are not exactly x86_64 and arm64" cputype arm64 0x01000012
tcase "an alignment over 2^15 fails" "slices: the arm64 slice lies outside the file or is not aligned" align arm64 16
tcase "an offset that is not aligned fails" "slices: the x86_64 slice lies outside the file or is not aligned" offset x86_64 0x4001
tcase "a slice past the end of the file fails" "slices: the arm64 slice lies outside the file or is not aligned" size arm64 0x7fffffff
tcase "an empty slice fails" "slices: the arm64 slice lies outside the file or is not aligned" size arm64 0
tcase "overlapping slices fail" "slices: the library's slices overlap" offset arm64 0x4000
tcase "swapped architectures fail" "slices: the x86_64 slice's header names another architecture" swap
tcase "a slice too small for a header fails" "slices: the x86_64 slice is too small for a Mach-O header" truncate x86_64 16
tcase "a 32-bit Mach-O slice fails" "slices: the x86_64 slice is not a 64-bit Mach-O dynamic library" magic x86_64 0xfeedface
tcase "a slice that is not a dynamic library fails" "slices: the arm64 slice is not a 64-bit Mach-O dynamic library" filetype arm64 2
tcase "load commands past the slice fail" "slices: the arm64 slice's load commands run past the slice" sizeofcmds arm64 0x100000
tcase "more than 4096 load commands fail" "slices: the x86_64 slice's load commands run past the slice" ncmds x86_64 5000
tcase "one load command more than the size holds fails" "slices: the x86_64 slice's load commands run past their size" ncmds x86_64 1
tcase "a load command of size 0 fails" "slices: a x86_64 load command has a size that does not fit" cmdsize x86_64 0
tcase "a load command size that is not a multiple of 8 fails" "slices: a arm64 load command has a size that does not fit" lastcmd-grow arm64 4
tcase "a load command larger than the commands fails" "slices: a x86_64 load command has a size that does not fit" cmdsize x86_64 0x10000
tcase "load commands that do not fill their size fail" "slices: the arm64 slice's load commands do not fill the size its header gives" sizeofcmds arm64 8
# A load command whose fields this tool reads, only 8 bytes long, so its fields would come from the
# next command: each kind fails as it is read.
for c in 0x80000022:48 0x32:24 0xc:24; do
  tlib "an 8-byte load command ${c%%:*} fails" "slices: a x86_64 load command ${c%%:*} is 8 bytes, shorter than its ${c#*:} bytes of fields" \
    "{\"command\": $((${c%%:*}))}"
done
# A form the policy refuses is no layout fault: an 8-byte one passes the slices rule, and the
# load-commands rule then names it (verify-macos-policy_test.sh).
python3 -I "$here/make_macho.py" "$tp" "{\"command\": $((0x80000034))}"; check "$tp"
expect "an 8-byte LC_DYLD_CHAINED_FIXUPS passes the slices rule, which reads no field of it" 0 ""

# A universal file over 1 GiB, sparse.
python3 -I -c 'import sys; f = open(sys.argv[1], "r+b"); f.seek(1 << 30); f.write(b"\0")' "$tp" 2>/dev/null ||
  { cp "$t" "$tp"; python3 -I -c 'import sys; f = open(sys.argv[1], "r+b"); f.seek(1 << 30); f.write(b"\0")' "$tp"; }
check "$tp"; rm -f "$tp"
expect "a file over 1 GiB fails" 1 "slices: the library is not a universal Mach-O file"
check "$tmp/no-such.dylib"
expect "an unreadable library fails" 1 "slices: cannot read $tmp/no-such.dylib"
printf 'not a library' > "$tmp/not.dylib"; check "$tmp/not.dylib"
expect "a file that is not a universal Mach-O fails" 1 "slices: the library is not a universal Mach-O file"
[[ -z "$(ls -A "$tmp/thin")" ]] && pass "thin writes nothing for a file that is not a universal library" ||
  fail "thin wrote $(ls "$tmp/thin") for a file that is not a universal library"
# The other commands apply the same rule first.
for cmd in policy exports; do
  args=("$tmp/not.dylib"); [[ "$cmd" == exports ]] || args+=("$odbc/policy/macos" "$header")
  out_text="$(python3 -I "$lib/macho.py" "$cmd" "${args[@]}" 2>&1)" && status=0 || status=$?
  expect "macho.py $cmd applies the slices rule first" 1 "slices: the library is not a universal Mach-O file"
done

# thin writes only new files, into a directory that exists.
rm -rf -- "${tmp:?}/thin"; mkdir "$tmp/thin"; printf 'kept' > "$tmp/thin/x86_64"
out_text="$(python3 -I "$lib/macho.py" thin "$t" "$tmp/thin" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out_text" == "cannot read or write a file: "*"File exists"* && "$(cat "$tmp/thin/x86_64")" == kept ]]; then
  pass "thin refuses a slice file that exists, and leaves it unchanged"
else fail "thin over an existing file (exit $status; $out_text)"; fi
out_text="$(python3 -I "$lib/macho.py" thin "$t" "$tmp/no-such-dir" 2>&1)" && status=0 || status=$?
expect "thin into a directory that does not exist fails" 1 "cannot read or write a file: "
out_text="$(python3 -I "$lib/macho.py" thin "$t" 2>&1)" && status=0 || status=$?
expect "thin without its directory is a usage error" 2 "usage: macho.py policy"

# Through verify_macos.sh, in a tree pinning the test PKI: a package that passes its own rules but
# holds a library that fails the slices rule fails at the loading-policy step.
if sha1_refused; then skip "the signed-package cases" "$no_sha1"; finish; fi
mpki="$tmp/mpki"; make_pki "$mpki" || { fail "a test PKI is generated"; finish; }
tree="$tmp/tree"; mac_tree "$tree" "$mpki"; serve_notices
pkg="$FAKE_CURL_DIR/AmazonRedshiftODBC-64-bit.$version.universal.pkg"
package_with() { # package_with <library file>
  python3 -I "$here/make_xar.py" "$pkg" "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\", \"$mpki/root.pem\"]}, \"members\": [[\"./librsodbc64.dylib\", \"file\", 1, \"@$1\"]]}"
  rm -f "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" bash "$tree/tools/lib/verify_macos.sh" osx_arm64 "$version" "$tree" "$header" 2>&1)" && status=0 || status=$?
}
package_with "$tmp/not.dylib"
expect "a signed package whose library is not a universal file fails at the loading-policy step" 1 \
  "verify-release: osx_arm64: slices: the library is not a universal Mach-O file"
patch "$t" "$tp" drop arm64; package_with "$tp"
expect "a signed package whose library has one slice fails, naming the slices rule" 1 \
  "verify-release: osx_arm64: slices: the library has 1 slices; it must have exactly x86_64 and arm64"
# Each slice is checked as its own platform: a name only the x86_64 slice holds, ignored "only in
# osx_amd64", passes, and fails when the policy says osx_arm64 instead; held by both slices, it
# fails at the arm64 one.
python3 -I "$here/make_macho.py" "$tp" "{\"exports\": $(entry_points_json), \"strings\": [\"SSL_CERT_FILE\", \"SSL_SLICE_ONLY\"], \"arm64_strings\": [\"SSL_CERT_FILE\"]}"
echo "SSL_SLICE_ONLY  a test string  # only in osx_amd64" > "$tree/policy/ignored-strings.txt"
package_with "$tp"
expect "a name only the x86_64 slice holds passes when the policy gives it to osx_amd64" 0 "osx_amd64  driver  librsodbc64.dylib"
echo "SSL_SLICE_ONLY  a test string  # only in osx_arm64" > "$tree/policy/ignored-strings.txt"
package_with "$tp"
expect "the same name fails when the policy gives it to osx_arm64 only" 1 \
  "x86_64: environment: the driver names SSL_SLICE_ONLY, whose row (ignored-strings.txt:1) is only in osx_arm64"
python3 -I "$here/make_macho.py" "$tp" "{\"exports\": $(entry_points_json), \"strings\": [\"SSL_CERT_FILE\", \"SSL_SLICE_ONLY\"]}"
echo "SSL_SLICE_ONLY  a test string  # only in osx_amd64" > "$tree/policy/ignored-strings.txt"
package_with "$tp"
expect "a name both slices hold fails at the arm64 slice when the policy gives it to osx_amd64 only" 1 \
  "arm64: environment: the driver names SSL_SLICE_ONLY, whose row (ignored-strings.txt:1) is only in osx_amd64"

finish
