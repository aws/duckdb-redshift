#!/usr/bin/env bash
# Tool tests for third_party/redshift_odbc/tools/lib/xar.py, which takes one file out of a macOS
# package's component payload. Each case writes a small package with make_xar.py, including
# members a normal user cannot create and tables of contents no packaging tool would write, and
# checks the exit status, the message and what was written. Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
reader="$here/../../../third_party/redshift_odbc/tools/lib/xar.py"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
lib=./AmazonRedshiftODBC.pkg/Payload/librsodbc64.dylib

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
BOUNDED_SECONDS=60 # a reader that loops on a bad payload has longer than a case that waits on a FIFO
# shellcheck source=bounded.sh
source "$here/bounded.sh" # bounded <command>...: at most $BOUNDED_SECONDS, its process group stopped
# check <name> <expected-exit> <expected-substring> <path-in-package> <json spec>: the out
# directory holds nothing after a failure, not even a temporary. The output stays in last_out.
check() {
  local name="$1" want="$2" needle="$3" path="$4" spec="$5" out status
  rm -rf -- "${tmp:?}/out"; mkdir "$tmp/out"
  python3 -I "$here/make_xar.py" "$tmp/p.pkg" "$spec"
  out="$(bounded python3 -I "$reader" extract "$tmp/p.pkg" "$path" "$tmp/out/lib" sha1 2>&1)" && status=0 || status=$?
  last_out="$out"
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]] &&
    { [[ "$want" == 0 ]] || [[ -z "$(ls -A "$tmp/out")" ]]; }; then pass "$name"
  else fail "$name (exit $status, wanted $want; output: $out)"; fi
}
# same <a> <b>: the two files hold the same bytes (in Python, as cmp may be missing).
same() { python3 -I -c 'import sys; sys.exit(open(sys.argv[1], "rb").read() != open(sys.argv[2], "rb").read())' "$1" "$2"; }
data_is() { [[ "$(cat "$tmp/out/lib")" == "$1" ]] && pass "$2" || fail "$2 ($(head -c 80 "$tmp/out/lib" 2>&1))"; }

check "takes the one regular file from an odc payload, gzip compressed" 0 "" "$lib" '{}'
data_is "data of ./librsodbc64.dylib" "writes that member's data"
head -c 20000 /dev/urandom > "$tmp/big"
check "takes a member of many chunks" 0 "" "$lib" "{\"members\": [[\".\", \"dir\", 2], [\"./librsodbc64.dylib\", \"file\", 1, \"@$tmp/big\"]]}"
same "$tmp/big" "$tmp/out/lib" && pass "writes the member unchanged" || fail "member bytes"
check "reads a payload the table of contents stores zlib-encoded" 0 "" "$lib" '{"encoding": "zlib"}'
check "reads a package whose Payload is at its top" 0 "" ./Payload/librsodbc64.dylib '{"component": ""}'
check "takes only the member asked for among others" 0 "" "$lib" \
  '{"members": [[".", "dir", 2], ["./odbc.ini", "file"], ["./librsodbc64.dylib", "file"], ["./connect", "file"]]}'
data_is "data of ./librsodbc64.dylib" "the member's data, not a neighbour's"
# Only the forms 2.3.0.0 uses: a header naming sha1, an odc payload, gzip.
check "refuses a header with the md5 checksum algorithm" 1 "the xar header has size 28 and checksum algorithm 2; this reader accepts only size 28 and sha1 (1)" "$lib" '{"alg": "md5"}'
check "refuses a header with no checksum algorithm" 1 "the xar header has size 28 and checksum algorithm 0" "$lib" '{"alg": "none"}'
check "refuses a header that names its checksum algorithm" 1 "the xar header has size 64 and checksum algorithm 3" "$lib" '{"alg": "sha256"}'
check "refuses a newc payload" 1 "the payload's cpio archive is not in the odc format" "$lib" '{"cpio": "newc"}'
# The 2.3.0.0 package's own shapes, so a default run, without the real package, covers them.
check "reads repeated equal <name> elements, signature elements and bytes after the heap, as 2.3.0.0 has" 0 "" "$lib" \
  '{"shape": ["repeated-names", "signature-elements", "heap-trailing"]}'
check "reads repeated names in a zlib-encoded Payload" 0 "" "$lib" \
  '{"shape": ["repeated-names"], "encoding": "zlib"}'
check "reads files nested 32 deep" 0 "" "$lib" '{"nest": 32}'

# The member checks, applied by lib/cpio.py to the payload.
check "refuses a symbolic link at the path" 1 "is not a regular file (mode 120777)" "$lib" '{"members": [["./librsodbc64.dylib", "link"]]}'
check "refuses a device at the path" 1 "is not a regular file" "$lib" '{"members": [["./librsodbc64.dylib", "dev"]]}'
check "refuses a FIFO at the path" 1 "is not a regular file" "$lib" '{"members": [["./librsodbc64.dylib", "fifo"]]}'
check "refuses a directory at the path" 1 "is not a regular file (mode 40755)" "$lib" '{"members": [["./librsodbc64.dylib", "dir"]]}'
check "refuses a hard-linked file at the path" 1 "has 2 links, not one" "$lib" '{"members": [["./librsodbc64.dylib", "file", 2]]}'
check "refuses two members at the path" 1 "more than one member" "$lib" \
  '{"members": [["./librsodbc64.dylib", "file"], ["./librsodbc64.dylib", "file"]]}'
check "refuses a ../ member before the path" 1 "member b'../escape' has an absolute path or a .. segment" "$lib" \
  '{"members": [["../escape", "file"], ["./librsodbc64.dylib", "file"]]}'
check "refuses an absolute member after the path was taken" 1 "member b'/etc/x' has an absolute path" "$lib" \
  '{"members": [["./librsodbc64.dylib", "file"], ["/etc/x", "file"]]}'
check "refuses a payload without the path" 1 "no member at ./librsodbc64.dylib" "$lib" '{"members": [["./other", "file"]]}'

# The package itself.
check "refuses a file that is not a xar archive" 1 "not a xar archive" "$lib" '{"break": ["magic"]}'
check "refuses Payload bytes whose checksum differs from the table of contents" 1 "archived checksum does not match" "$lib" '{"break": ["payload-checksum"]}'
check "refuses decoded Payload bytes whose checksum differs" 1 "extracted checksum does not match" "$lib" '{"encoding": "zlib", "break": ["extracted-checksum"]}'
check "refuses an odc header field that is not octal" 1 "not an odc cpio archive, or a header field is not octal" "$lib" '{"break": ["odc-not-octal"]}'
check "refuses a Payload location outside the file" 1 "lies outside the file" "$lib" '{"break": ["payload-outside"]}'
check "refuses a Payload with two different names" 1 "names that differ" "$lib" '{"break": ["names-differ"]}'
check "refuses two files at one path in the table of contents" 1 "two files at AmazonRedshiftODBC.pkg/Payload" "$lib" '{"break": ["two-payloads"]}'
check "refuses a document type declaration" 1 "document type declaration" "$lib" '{"break": ["doctype"]}'
check "refuses a document type that declares nothing" 1 "document type declaration" "$lib" '{"break": ["doctype-bare"]}'
check "refuses an entity that would name Payload" 1 "document type declaration" "$lib" '{"break": ["entity-used"]}'
check "refuses a document type in a UTF-16 table of contents" 1 "document type declaration" "$lib" '{"break": ["doctype-utf16"]}'
check "refuses a table of contents of another size than the header's" 1 "does not have the size the header gives" "$lib" '{"break": ["toc-size"]}'
check "refuses a Payload that is a directory" 1 "is a directory, not a file" "$lib" '{"break": ["payload-directory"]}'
check "refuses an encoding it does not read" 1 "encoding 'application/x-bzip2'" "$lib" '{"break": ["encoding-bzip2"]}'
check "refuses a payload compression it does not read" 1 "the payload is not gzip compressed" "$lib" '{"compress": "bzip2"}'
check "refuses data after the payload's gzip stream" 1 "data follows the payload's gzip stream" "$lib" '{"break": ["gzip-trailing"]}'
check "refuses a path that is not inside a Payload" 1 "does not name a file inside a component's Payload" ./AmazonRedshiftODBC.pkg/librsodbc64.dylib '{}'
check "refuses a path that names the Payload itself" 1 "does not name a file inside a component's Payload" ./AmazonRedshiftODBC.pkg/Payload '{}'
check "refuses a path outside the lock's grammar" 1 "path in the package must be" ./AmazonRedshiftODBC.pkg/Payload/../x '{}'
check "refuses a component the package does not have" 1 "the package has no Other.pkg/Payload" ./Other.pkg/Payload/librsodbc64.dylib '{}'

# The header.
check "refuses a file shorter than the header" 1 "not a xar archive" "$lib" '{"break": ["short"]}'
check "refuses header version 2" 1 "xar header version 2 is not 1" "$lib" '{"break": ["version"]}'
check "refuses a header of another size with the sha1 algorithm" 1 "has size 32 and checksum algorithm 1" "$lib" '{"break": ["header-size"]}'
check "refuses an unknown checksum algorithm number" 1 "has size 28 and checksum algorithm 4" "$lib" '{"break": ["header-algorithm"]}'
check "refuses a table of contents decoded size over the bound" 1 "sizes are out of bounds" "$lib" '{"break": ["toc-size-bound"]}'
check "refuses a table of contents stored size over the bound" 1 "sizes are out of bounds" "$lib" '{"break": ["toc-length-bound"]}'
check "refuses a table of contents that runs past the file" 1 "runs past the end of the file" "$lib" '{"break": ["toc-past-end"]}'
check "refuses data after the table of contents' zlib stream" 1 "or data follows it" "$lib" '{"break": ["toc-trailing"]}'
check "refuses a cut table of contents whose size the header matches" 1 "or data follows it" "$lib" '{"break": ["toc-truncated"]}'
check "refuses a table of contents that is not a zlib stream" 1 "the table of contents is not a zlib stream" "$lib" '{"break": ["toc-not-zlib"]}'

# The table of contents.
check "refuses a table of contents that is not well-formed XML" 1 "is not well-formed XML" "$lib" '{"break": ["toc-not-xml"]}'
check "refuses text that is not XML before the root element" 1 "is not well-formed XML" "$lib" '{"break": ["prolog-not-xml"]}'
check "refuses a root element other than <xar>" 1 "is not a <xar> document" "$lib" '{"break": ["root-tag"]}'
check "refuses a <xar> without <toc>" 1 "<xar> has 0 <toc> elements, not one" "$lib" '{"break": ["no-toc"]}'
check "refuses files nested 33 deep" 1 "nests files deeper than 32" "$lib" '{"nest": 33}'
check "refuses a file without a name" 1 "has no name, or names that differ: []" "$lib" '{"break": ["no-name"]}'
check "refuses one top-level file named AmazonRedshiftODBC.pkg/Payload" 1 "has the name 'AmazonRedshiftODBC.pkg/Payload'" "$lib" '{"break": ["name-flat"]}'
check "refuses a file named .." 1 "has the name '..'" "$lib" '{"break": ["name-dotdot"]}'
check "refuses a file named ." 1 "has the name '.'" "$lib" '{"break": ["name-dot"]}'
check "refuses a file with an empty name" 1 "has the name ''" "$lib" '{"break": ["name-empty"]}'
check "refuses a Payload with two <data> elements" 1 "has 2 <data> elements, not one" "$lib" '{"break": ["two-data"]}'
check "refuses an offset that is not a decimal number" 1 "<offset> is not a number" "$lib" '{"break": ["offset-not-number"]}'
check "refuses a Payload of another size than the table of contents gives" 1 "not the size the table of contents gives" "$lib" '{"break": ["payload-size"]}'
check "refuses a checksum algorithm it does not know" 1 "uses the algorithm 'crc32', which is not accepted" "$lib" '{"break": ["checksum-style-unknown"]}'
check "refuses a zlib-encoded Payload that is not zlib" 1 "is not the zlib stream its encoding names" "$lib" '{"encoding": "zlib", "break": ["zlib-not-zlib"]}'
check "refuses a cut zlib-encoded Payload" 1 "zlib stream is truncated or followed by data" "$lib" '{"encoding": "zlib", "break": ["zlib-truncated"]}'
check "refuses data after a zlib-encoded Payload" 1 "zlib stream is truncated or followed by data" "$lib" '{"encoding": "zlib", "break": ["zlib-trailing"]}'

# The payload's compression and its odc archive.
check "refuses a cut gzip payload, and stops" 1 "the payload's gzip stream is truncated" "$lib" '{"break": ["gzip-truncated"]}'
check "refuses an odc header with another magic" 1 "not an odc cpio archive" "$lib" '{"break": ["odc-magic"]}'
# What follows the odc trailer reaches cpio.py's trailer rules, in either compression.
check "refuses data after the odc trailer" 1 "data follows the archive's trailer" "$lib" '{"break": ["odc-trailing"]}'
check "refuses more than 4096 zero bytes after the odc trailer" 1 "more than 4096 bytes follow the archive's trailer" "$lib" \
  '{"break": ["odc-padding"]}'
check "refuses an odc mtime over 32 bits" 1 "a member's size or name size is out of bounds" "$lib" '{"break": ["odc-mtime"]}'
check "refuses an odc size over 32 bits" 1 "a member's size or name size is out of bounds" "$lib" '{"break": ["odc-size"]}'
check "refuses an odc name size of 0" 1 "a member's size or name size is out of bounds" "$lib" '{"break": ["odc-name-size-zero"]}'
check "refuses an odc name size over 4096" 1 "a member's size or name size is out of bounds" "$lib" '{"break": ["odc-name-size-long"]}'
check "refuses an odc archive cut inside a member" 1 "the archive ends inside a member" "$lib" '{"break": ["odc-truncated"]}'

# Every file's decoded size is bounded when the table of contents is read, before anything is
# decompressed: 64 MiB a file, 128 MiB all together. A refused size names the file and fails in one
# line, whether the file is read or not.
mib=$((1 << 20))
check "a file that declares exactly 64 MiB passes the size bound" 0 "" "$lib" "{\"big\": [[\"Big\", $((64 * mib)), 1]]}"
check "a file that declares one byte over 64 MiB fails" 1 \
  "the table of contents gives Big a decoded size of $((64 * mib + 1)) bytes, more than the $((64 * mib)) a file may have" \
  "$lib" "{\"big\": [[\"Big\", $((64 * mib + 1)), 1]]}"
check "files that decode to exactly 128 MiB together pass" 0 "" "$lib" \
  "{\"big\": [[\"Big1\", $((64 * mib)), 1], [\"Big2\", \"rest:$((128 * mib))\", 1]]}"
check "files that decode to one byte over 128 MiB together fail" 1 \
  "the table of contents' files decode to more than $((128 * mib)) bytes together" \
  "$lib" "{\"big\": [[\"Big1\", $((64 * mib)), 1], [\"Big2\", \"rest:$((128 * mib + 1))\", 1]]}"
# The next four cases run under an address-space limit, which macOS refuses: there the subshell
# exits 70 and each case skips, as in common_test.sh.
# extract_limited <package> <path>: xar.py extract into $tmp/out/lib under a 512 MiB limit; sets out
# and status.
extract_limited() {
  out="$( (ulimit -v 524288 2> /dev/null || exit 70; python3 -I "$reader" extract "$1" "$2" "$tmp/out/lib" sha1) 2>&1)" &&
    status=0 || status=$?
}
limited() { # <status> <title> <0 if the case's condition held> <failure text>
  if [[ "$1" == 70 ]]; then n=$((n+1)); echo "ok $n - $2 # SKIP this shell cannot set ulimit -v"
  elif [[ "$3" == 0 ]]; then pass "$2"; else fail "$4"; fi
}
# High expansion: a zlib-encoded Payload whose 1 MB stored stream holds 1 GiB of zeros, its decoded
# size declared honestly. It fails at the bound under an address-space limit that zlib's output
# could not fit in, so it was refused before anything was decompressed.
python3 -I "$here/make_xar.py" "$tmp/expand.pkg" "{\"big\": [[\"Big.pkg/Payload\", $((1024 * mib)), $((1024 * mib))]]}"
rm -rf -- "${tmp:?}/out"; mkdir "$tmp/out"
extract_limited "$tmp/expand.pkg" ./Big.pkg/Payload/librsodbc64.dylib
[[ "$status" == 1 && "$out" == "xar.py: the table of contents gives Big.pkg/Payload a decoded size of $((1024 * mib)) bytes, more than the $((64 * mib)) a file may have" &&
  "$(python3 -I -c 'import os,sys; print(os.path.getsize(sys.argv[1]) < (4 << 20))' "$tmp/expand.pkg")" == True && -z "$(ls -A "$tmp/out")" ]] &&
  held=0 || held=1
limited "$status" "a 1 MB stream that expands to 1 GiB fails at the bound, under a 512 MiB address-space limit, before zlib runs" \
  "$held" "a high-expansion Payload (exit $status: $out)"
# The same stream with a decoded size under the bound declared: zlib is asked for at most that size
# plus one byte, so the stream fails as longer than declared, still under the limit.
python3 -I "$here/make_xar.py" "$tmp/expand.pkg" "{\"big\": [[\"Big.pkg/Payload\", 1000, $((1024 * mib))]]}"
extract_limited "$tmp/expand.pkg" ./Big.pkg/Payload/librsodbc64.dylib
[[ "$status" == 1 && "$out" == "xar.py: Big.pkg/Payload's zlib stream is truncated or followed by data" ]] && held=0 || held=1
limited "$status" "a stream that expands past its declared size fails without decoding more than that size" \
  "$held" "an under-declared high-expansion Payload (exit $status: $out)"
# The table of contents is read before any signature, so its decompression is bounded by the size
# the header gives too: a 1 GiB tail of spaces behind a valid table of contents, about 1 MB stored,
# fails as data after it, under the same limit.
python3 -I "$here/make_xar.py" "$tmp/p.pkg" '{}'
python3 -I -c 'import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
size, version, length, toc_size, alg = struct.unpack(">HHQQI", d[4:28])
xml = zlib.decompress(d[size:size + length])
c = zlib.compressobj(9)
toc = c.compress(xml) + b"".join(c.compress(b" " * (1 << 20)) for _ in range(1024)) + c.flush()
open(sys.argv[2], "wb").write(d[:4] + struct.pack(">HHQQI", size, version, len(toc), toc_size, alg) + toc + d[size + length:])' \
  "$tmp/p.pkg" "$tmp/expand.pkg"
extract_limited "$tmp/expand.pkg" "$lib"
[[ "$status" == 1 && "$out" == "xar.py: the table of contents does not have the size the header gives, or data follows it" ]] &&
  held=0 || held=1
limited "$status" "a table of contents that expands past its declared size fails without decoding the rest, under a 512 MiB limit" \
  "$held" "a high-expansion table of contents (exit $status: $out)"
# The payload's gzip stream is read in 1 MiB pieces as cpio.py asks for them: a 600 MiB member at
# another path streams through under the same limit, and the run fails only for the missing path.
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(600 << 20)' "$tmp/zeros"
python3 -I "$here/make_xar.py" "$tmp/expand.pkg" "{\"members\": [[\"./other\", \"file\", 1, \"@$tmp/zeros\"]]}"
rm -f -- "$tmp/zeros"
extract_limited "$tmp/expand.pkg" "$lib"
[[ "$status" == 1 && "$out" == *"no member at ./librsodbc64.dylib"* && "$out" != *MemoryError* ]] && held=0 || held=1
limited "$status" "a 600 MiB payload member streams through cpio.py under a 512 MiB limit" \
  "$held" "a large payload member under a memory limit (exit $status: $out)"
rm -f -- "$tmp/expand.pkg"

# find and product, run directly: each takes the accepted algorithm, find then the names.
xar() { out="$(python3 -I "$reader" "$@" 2>&1)" && status=0 || status=$?; }
python3 -I "$here/make_xar.py" "$tmp/p.pkg" '{"payload_alg": "md5", "members": [["./librsodbc64.dylib", "file"], ["./LICENSE", "file"]]}'
xar find "$tmp/p.pkg" sha1
[[ "$status" == 2 && "$out" == "usage: "* ]] && pass "find without a name is a usage error" || fail "find without a name (exit $status: $out)"
xar find "$tmp/p.pkg" sha1 LICENSE
[[ "$status" == 1 && "$out" == "xar.py: AmazonRedshiftODBC.pkg/Payload's archived checksum uses the algorithm 'md5', which is not accepted" ]] &&
  pass "find refuses a Payload whose checksums use another algorithm than the one it is given" || fail "find and an md5 Payload (exit $status: $out)"
rm -rf -- "${tmp:?}/out"; mkdir "$tmp/out"
xar extract "$tmp/p.pkg" "$lib" "$tmp/out/lib" sha1
[[ "$status" == 1 && "$out" == *"archived checksum uses the algorithm 'md5', which is not accepted" && -z "$(ls -A "$tmp/out")" ]] &&
  pass "extract refuses a Payload whose checksums use another algorithm" || fail "extract and an md5 Payload (exit $status: $out)"
xar extract "$tmp/p.pkg" "$lib" "$tmp/out/lib" md5
[[ "$status" == 1 && "$out" == *"archived checksum uses the algorithm 'md5', which is not accepted" && -z "$(ls -A "$tmp/out")" ]] &&
  pass "extract given an algorithm it does not compute refuses even a Payload in it" || fail "extract given md5 (exit $status: $out)"
xar extract "$tmp/p.pkg" "$lib" "$tmp/out/lib"
[[ "$status" == 2 && "$out" == "usage: "* ]] && pass "extract without an algorithm is a usage error" || fail "extract without an algorithm (exit $status: $out)"
xar extract "$tmp/p.pkg" "$lib" "$tmp/out/lib" sha1 sha256
[[ "$status" == 2 && "$out" == "usage: "* ]] && pass "extract with two algorithms is a usage error" || fail "extract with two algorithms (exit $status: $out)"
python3 -I "$here/make_xar.py" "$tmp/p.pkg" '{"members": [["./librsodbc64.dylib", "file"], ["./LICENSE", "file"], ["./a/NOTICE", "file"]]}'
xar find "$tmp/p.pkg" sha1 LICENSE NOTICE
[[ "$status" == 0 && "$out" == "LICENSE ./AmazonRedshiftODBC.pkg/Payload/LICENSE"$'\n'"NOTICE ./AmazonRedshiftODBC.pkg/Payload/a/NOTICE" ]] &&
  pass "find lists the members with those names" || fail "find (exit $status: $out)"
python3 -I "$here/make_xar.py" "$tmp/p.pkg" '{"component": ""}'
xar product "$tmp/p.pkg" ./Payload/librsodbc64.dylib com.amazon.redshift.odbc2x64 2.3.0.0 sha1
[[ "$status" == 0 && -z "$out" ]] && pass "product reads the PackageInfo beside a Payload at the package's top" ||
  fail "product at the top (exit $status: $out)"
xar product "$tmp/p.pkg" ./Payload/librsodbc64.dylib com.amazon.redshift.odbc2x64 2.3.0.1 sha1
[[ "$status" == 1 && "$out" == "xar.py: product: PackageInfo names <pkg-info> identifier 'com.amazon.redshift.odbc2x64', version '2.3.0.0';"* ]] &&
  pass "product refuses another version" || fail "product and another version (exit $status: $out)"
xar product "$tmp/p.pkg" ./Payload/librsodbc64.dylib com.amazon.redshift.odbc2x64 2.3.0.0 md5
[[ "$status" == 1 && "$out" == "xar.py: product: PackageInfo's archived checksum uses the algorithm 'sha1', which is not accepted" ]] &&
  pass "product refuses a PackageInfo in another algorithm than the one it is given" || fail "product given md5 (exit $status: $out)"
xar product "$tmp/p.pkg" ./Payload/librsodbc64.dylib com.amazon.redshift.odbc2x64 2.3.0.0
[[ "$status" == 2 && "$out" == "usage: "* ]] && pass "product without an algorithm is a usage error" ||
  fail "product without an algorithm (exit $status: $out)"
# A PackageInfo that passes the prolog's document-type check and is still not well-formed: one line
# naming the rule, not a traceback.
python3 -I "$here/make_xar.py" "$tmp/p.pkg" \
  '{"component": "", "package_info": {"raw": "<pkg-info identifier=\"com.amazon.redshift.odbc2x64\" version=\"2.3.0.0\"><a></pkg-info>"}}'
xar product "$tmp/p.pkg" ./Payload/librsodbc64.dylib com.amazon.redshift.odbc2x64 2.3.0.0 sha1
[[ "$status" == 1 && "$out" == "xar.py: product: PackageInfo is not well-formed XML: mismatched tag"* && "$out" != *$'\n'* ]] &&
  pass "product refuses a PackageInfo that is not well-formed after its prolog, in one line" ||
  fail "product and a mismatched tag (exit $status: $out)"

python3 -I "$here/make_xar.py" "$tmp/p.pkg" '{}'
size="$(python3 -I -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$tmp/p.pkg")"
python3 -I -c 'import sys; p = sys.argv[1]; d = bytearray(open(p, "rb").read()); d[int(sys.argv[2]) - 60] ^= 1; open(p, "wb").write(d)' "$tmp/p.pkg" "$size"
rm -rf -- "${tmp:?}/out"; mkdir "$tmp/out"
out="$(python3 -I "$reader" extract "$tmp/p.pkg" "$lib" "$tmp/out/lib" sha1 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == *"checksum does not match"* && -z "$(ls -A "$tmp/out")" ]] &&
  pass "one changed heap byte fails its checksum" || fail "one changed heap byte (exit $status: $out)"
rm -rf -- "${tmp:?}/out"; mkdir "$tmp/out"
out="$(python3 -I "$reader" extract "$tmp/missing.pkg" "$lib" "$tmp/out/lib" sha1 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == *"cannot read the package $tmp/missing.pkg"* ]] && pass "a missing package fails, naming it" ||
  fail "a missing package (exit $status: $out)"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.seek(256 << 20); f.write(b"\0")' "$tmp/big.pkg"
out="$(python3 -I "$reader" extract "$tmp/big.pkg" "$lib" "$tmp/out/lib" sha1 2>&1)" && status=0 || status=$?
rm -f -- "${tmp:?}/big.pkg"
[[ "$status" == 1 && "$out" == *"the package is larger than 268435456 bytes"* ]] && pass "a package over 256 MiB fails" ||
  fail "a package over 256 MiB (exit $status: $out)"
python3 -I "$here/make_xar.py" "$tmp/p.pkg" '{}'
: > "$tmp/out/lib"
out="$(python3 -I "$reader" extract "$tmp/p.pkg" "$lib" "$tmp/out/lib" sha1 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == *"File exists"* ]] && pass "never writes over an existing file" ||
  fail "never writes over an existing file ($status: $out)"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
