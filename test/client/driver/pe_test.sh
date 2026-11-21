#!/usr/bin/env bash
# Tool tests for third_party/redshift_odbc/tools/lib/pe.py, which lists a Windows image's RCDATA
# resources. Each case writes a small image with make_pe.py and checks the exit status and the
# output. With RS_LOAD_ONLY_EXTENSION naming a copy of the loadable extension of a windows_amd64
# build without the embedded driver, the last case shows that it holds no RCDATA resource. Run by
# run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
reader="$here/../../../third_party/redshift_odbc/tools/lib/pe.py"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# shellcheck source=bounded.sh
source "$here/bounded.sh" # bounded <command>...: at most $BOUNDED_SECONDS, its process group stopped
# check <name> <expected-exit> <expected-output> <json spec>: the reader's whole output, stdout and
# stderr, must equal <expected-output>.
check() {
  local name="$1" want="$2" expected="$3" spec="$4" out status
  python3 -I "$here/make_pe.py" "$tmp/image.dll" "$spec"
  out="$(bounded python3 -I "$reader" rcdata "$tmp/image.dll" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == "$expected" ]]; then pass "$name"
  else fail "$name (exit $status, wanted $want; output: $out)"; fi
}

check "lists each RCDATA resource by number or string name, with its language and size" 0 \
  $'101 0 12\n"DRIVER_SET" 1033 3' \
  '{"resources": [[10, 101, 0, "driver bytes"], [10, "DRIVER_SET", 1033, "set"]]}'
check "reads past a manifest and a version resource, listing only the RCDATA between them" 0 '7 1033 5' \
  '{"resources": [[24, 2, 1033, "<assembly/>"], [10, 7, 1033, "bytes"], [16, 1, 1033, "version"]]}'
check "prints nothing for an image whose resources are of other types" 0 '' \
  '{"resources": [[24, 2, 1033, "<assembly/>"], [16, 1, 1033, "version"]]}'
check "prints nothing for an image with no resource directory, and reads past bytes after the image" 0 '' \
  '{"trailing": "an extension metadata footer"}'
check "lists a resource that has two languages once per language" 0 $'3 1033 1\n3 1031 2' \
  '{"resources": [[10, 3, 1033, "a"], [10, 3, 1031, "bb"]]}'
check "lists a string name outside ASCII escaped, as a JSON string" 0 '"\u00e9" 0 1' \
  '{"resources": [[10, "\u00e9", 0, "x"]]}'
check "reads data past the section's virtual size that the file holds" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "virtual_size": 16}'
check "reads no resource directory from an optional header one byte short of its entry" 0 '' \
  '{"resources": [[10, 1, 0, "x"]], "optional_size": 135}'
check "reads the resource directory from an optional header that just holds its entry" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "optional_size": 136}'
check "reads the resource directory from an image with exactly three data directories" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "directories": 3}'
check "finds the resource directory in the third of four sections" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "sections_before": 2}'
check "lists a string name that is not valid UTF-16 with U+FFFD in its place" 0 '"\ufffd" 0 1' \
  '{"resources": [[10, "N", 0, "x"]], "break": ["name-surrogate"]}'

check "refuses a file that is not a PE image" 1 \
  "image: not a PE image (no PE signature at the offset the MZ header gives)" \
  '{"break": ["no-pe"]}'
check "refuses a file that ends inside its headers" 1 "image: the COFF header lies outside the file" \
  '{"break": ["truncated"]}'
check "refuses a PE32 image" 1 "image: a PE32 (32-bit) image, not PE32+" '{"magic": 267}'
check "refuses an optional header that is neither PE32 nor PE32+" 1 \
  "image: the optional header's magic is 0x107, not PE32+ (0x20b)" '{"magic": 263}'
check "refuses an image for another machine" 1 "image: built for machine 0x14c, not x86-64 (0x8664)" '{"machine": 332}'
check "refuses a directory whose entries run past the resource directory" 1 \
  "resources: the root directory's entries run past the resource directory" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["entries-past"]}'
# The section starts at 0x1000 (4096) and, rounded to the file's 0x200, ends at 0x1200 (4608).
check "reads resource data at the start of its section" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "data_address": 4096}'
check "refuses resource data one byte below its section" 1 "resources: RCDATA 1's data lies outside every section" \
  '{"resources": [[10, 1, 0, "x"]], "data_address": 4095}'
check "reads resource data that ends at its section's end" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "data_address": 4607}'
check "refuses resource data that ends one byte past its section" 1 \
  "resources: RCDATA 1's data lies outside every section" \
  '{"resources": [[10, 1, 0, "x"]], "data_address": 4607, "data_size": 2}'
check "reads resource data that ends at the bytes a larger section holds in the file" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "virtual_size": 8192, "data_address": 4607}'
check "refuses resource data one byte past the bytes its section holds in the file" 1 \
  "resources: RCDATA 1's data lies outside the bytes its section holds in the file" \
  '{"resources": [[10, 1, 0, "x"]], "virtual_size": 8192, "data_address": 4607, "data_size": 2}'
check "refuses resource data in another section past the bytes it holds in the file" 1 \
  "resources: RCDATA 1's data lies outside the bytes its section holds in the file" \
  '{"resources": [[10, 1, 0, "x"]], "sections_before": 2, "data_address": 65536}'
check "refuses a fourth directory level" 1 "resources: RCDATA 1 has a fourth directory level" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["fourth-level"]}'
check "refuses a section that ends one byte past the end of the file" 1 \
  "resources: the resource directory lies outside the bytes its section holds in the file" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["section-cut"]}'
check "checks the directory of a type it does not list" 1 \
  "resources: a type's directory lies outside the resource directory" \
  '{"resources": [[24, 2, 1033, "m"], [10, 1, 0, "x"]], "break": ["subdir-outside"]}'
check "refuses a data entry outside the resource directory" 1 \
  "resources: RCDATA 1's data entry lies outside the resource directory" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["entry-outside"]}'
check "refuses a type directory outside the resource directory" 1 \
  "resources: a type's directory lies outside the resource directory" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["subdir-outside"]}'
check "refuses a type that points at a data entry" 1 \
  "resources: a type is a data entry, where the directory needs a level below it" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["type-data"]}'
check "refuses a name outside the resource directory" 1 "resources: a name lies outside the resource directory" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["name-outside"]}'
check "refuses a name whose length runs past the resource directory" 1 \
  "resources: a name runs past the resource directory" '{"resources": [[10, "N", 0, "x"]], "break": ["name-long"]}'
check "refuses a name that points at a data entry" 1 \
  "resources: a name is a data entry, where the directory needs a level below it" \
  '{"resources": [[10, 1, 0, "x"]], "break": ["name-data"]}'
# A numeric ID is 16 bits: 65546, 65537 and 66569 are 10, 1 and 1033 with bit 16 set.
check "refuses a type ID with reserved bits set" 1 "resources: a numeric ID has reserved bits set" \
  '{"resources": [[65546, 1, 0, "x"]]}'
check "refuses a name ID with reserved bits set" 1 "resources: a numeric ID has reserved bits set" \
  '{"resources": [[10, 65537, 0, "x"]]}'
check "refuses a language ID with reserved bits set" 1 "resources: a numeric ID has reserved bits set" \
  '{"resources": [[10, 1, 66569, "x"]]}'
# 1073741834 is 0x4000000A: type 10 with bit 30, the highest reserved bit, set.
check "refuses a type ID with bit 30 set" 1 "resources: a numeric ID has reserved bits set" \
  '{"resources": [[1073741834, 1, 0, "x"]]}'

# Each bound of the resource directory, one byte inside and one outside, by its size. One manifest:
# the root at 0, its type's name directory at 24 (16 bytes, then one 8-byte entry), and a name's
# language directory at 48, which rcdata does not read for a type it does not list.
manifest='"resources": [[24, 1, 1033, "m"]]'
check "refuses a directory whose header ends one byte past the resource directory" 1 \
  "resources: a type's directory lies outside the resource directory" "{$manifest, \"directory_size\": 39}"
check "reads a directory's entries once its header ends at the resource directory's end" 1 \
  "resources: a type's directory's entries run past the resource directory" "{$manifest, \"directory_size\": 40}"
check "refuses a directory whose entries end one byte past the resource directory" 1 \
  "resources: a type's directory's entries run past the resource directory" "{$manifest, \"directory_size\": 47}"
check "reads a directory whose entries end at the resource directory's end" 0 '' "{$manifest, \"directory_size\": 48}"
# One RCDATA resource: its data entry at 72 (16 bytes), then, when named, its name at 88 (a 2-byte
# length and one UTF-16 unit).
check "refuses a data entry that ends one byte past the resource directory" 1 \
  "resources: RCDATA 1's data entry lies outside the resource directory" \
  '{"resources": [[10, 1, 0, "x"]], "directory_size": 87}'
check "reads a data entry that ends at the resource directory's end" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "directory_size": 88}'
check "refuses a name whose length ends one byte past the resource directory" 1 \
  "resources: a name lies outside the resource directory" '{"resources": [[10, "N", 0, "x"]], "directory_size": 89}'
check "reads a name's text once its length ends at the resource directory's end" 1 \
  "resources: a name runs past the resource directory" '{"resources": [[10, "N", 0, "x"]], "directory_size": 90}'
check "refuses a name that ends one byte past the resource directory" 1 \
  "resources: a name runs past the resource directory" '{"resources": [[10, "N", 0, "x"]], "directory_size": 91}'
check "reads a name that ends at the resource directory's end" 0 '"N" 0 1' \
  '{"resources": [[10, "N", 0, "x"]], "directory_size": 92}'
# The one-resource section rounds to 512 bytes in the file, which its virtual size does not reach.
check "reads a resource directory that ends at its section's end" 0 '1 0 1' \
  '{"resources": [[10, 1, 0, "x"]], "directory_size": 512}'
check "refuses a resource directory one byte past its section" 1 \
  "resources: the resource directory lies outside every section" \
  '{"resources": [[10, 1, 0, "x"]], "directory_size": 513}'
check "reads no resource directory from an image with fewer than three data directories" 0 '' \
  '{"resources": [[10, 1, 0, "x"]], "directories": 2}'
# 1 + 32,767 + 32,768: the root's one type, its names, and their languages (name 1 has two).
python3 -I "$here/make_pe.py" "$tmp/image.dll" '{"resources": [[10, 1, 1033, "y"]], "many": 32767}'
out="$(bounded python3 -I "$reader" rcdata "$tmp/image.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 0 && "$(printf '%s\n' "$out" | wc -l)" -eq 32768 ]]; then
  pass "accepts exactly 65,536 entries in all"
else fail "accepts exactly 65,536 entries in all (exit $status; $(printf '%s' "$out" | head -c 200))"; fi
# 32,768 resources make 65,537 entries: the root's one type, its 32,768 names and their languages.
check "refuses a directory of more than 65,536 entries in all" 1 \
  "resources: the directory has more than 65536 entries" \
  '{"many": 32768}'
check "counts string-named entries toward the 65,536" 1 "resources: the directory has more than 65536 entries" \
  '{"many": 32768, "named": true}'
check "lists the entries a named many adds by their string names" 0 $'"R1" 0 1\n"R2" 0 1' \
  '{"many": 2, "named": true}'
# Resources that run past 0x8000, .rdata's usual address, as 2.3.1.0's larger notices do in the
# rename tests' sets: the writer puts .rdata after .rsrc's last page, so the two do not overlap and
# both the resources and the imports read back.
python3 -I "$here/make_pe.py" "$tmp/image.dll" "$(python3 -I -c 'import json
print(json.dumps({"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"]}, "resources": [[10, 1, 0, "x" * 0x8000]]}))')"
out="$(bounded python3 -I -B -c 'import importlib.util, sys
spec = importlib.util.spec_from_file_location("pe", sys.argv[1])
pe = importlib.util.module_from_spec(spec); spec.loader.exec_module(pe)
image = pe.Image(open(sys.argv[2], "rb").read())
named = {image.data[s.header:s.header + 8].rstrip(b"\0").decode(): s for s in image.sections}
rsrc, rdata = named[".rsrc"], named[".rdata"]
print(rsrc.address + rsrc.virtual_size > 0x8000, rdata.address >= rsrc.address + rsrc.virtual_size)
print(" ".join(i.name.decode() for i in pe.imports(image)))
print(" ".join("%s %d %d" % (label, language, size) for label, language, size, _ in pe.rcdata(image)))' \
  "$reader" "$tmp/image.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 0 && "$out" == $'True True\nKERNEL32.dll crypto.dll\n1 0 32768' ]]; then
  pass "an image whose resources run past 0x8000 holds .rdata after .rsrc, and both read back"
else fail "an image whose resources run past 0x8000 holds .rdata after .rsrc, and both read back (exit $status; $out)"; fi
printf '\177ELF\002\001\001' > "$tmp/elf"
out="$(python3 -I "$reader" rcdata "$tmp/elf" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "image: not a PE image (no MZ header)" ]]; then pass "refuses an ELF file"
else fail "refuses an ELF file (exit $status; $out)"; fi
out="$(python3 -I "$reader" rcdata 2>&1)" && status=0 || status=$?
if [[ "$status" == 2 && "$out" == "usage: pe.py rcdata <image>" ]]; then pass "prints its usage without an image"
else fail "prints its usage without an image (exit $status; $out)"; fi
out="$(python3 -I "$reader" list "$tmp/image.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 2 && "$out" == "usage: pe.py rcdata <image>" ]]; then pass "prints its usage for another command"
else fail "prints its usage for another command (exit $status; $out)"; fi
# A sparse file one byte over the bound, so it takes no disk, made with Python because macOS has no
# truncate(1); refused from its size.
python3 -I -c 'import sys; open(sys.argv[1], "wb").truncate((1 << 30) + 1)' "$tmp/huge.dll"
out="$(bounded python3 -I "$reader" rcdata "$tmp/huge.dll" 2>&1)" && status=0 || status=$?
rm -f "$tmp/huge.dll"
if [[ "$status" == 1 && "$out" == "image: the file is larger than 1 GiB" ]]; then pass "refuses a file larger than 1 GiB"
else fail "refuses a file larger than 1 GiB (exit $status; $out)"; fi
out="$(python3 -I "$reader" rcdata "$tmp/missing.dll" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "image: cannot read $tmp/missing.dll: No such file or directory" ]]; then
  pass "refuses a file it cannot read"
else fail "refuses a file it cannot read (exit $status; $out)"; fi

# The loadable extension of a build without the embedded driver, copied off the Windows build,
# embeds no driver set.
if [[ -n "${RS_LOAD_ONLY_EXTENSION:-}" ]]; then
  out="$(bounded python3 -I "$reader" rcdata "$RS_LOAD_ONLY_EXTENSION" 2>&1)" && status=0 || status=$?
  if [[ "$status" == 0 && -z "$out" ]]; then pass "the load-only bundle's extension holds no RCDATA resource"
  else fail "the load-only bundle's extension holds no RCDATA resource (exit $status; $out)"; fi
else
  n=$((n+1))
  echo "ok $n - the load-only bundle's extension holds no RCDATA resource # SKIP RS_LOAD_ONLY_EXTENSION is not set"
fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
