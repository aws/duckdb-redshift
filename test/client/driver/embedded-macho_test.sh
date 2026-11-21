#!/usr/bin/env bash
# Checks the macOS embed: the assembler object holds a private external begin and end symbol per file of
# the set in __TEXT,__const, data only and 64-byte aligned, with no .subsections_via_symbols; each range has
# the lock's SHA-256; the loadable extension carries the set and exports nothing more than DuckDB's entry
# point; and sources.cmake's compile check refuses a universal build and the other architecture. Apple's nm
# and otool read the objects (a GNU nm earlier on PATH cannot). make test_driver runs it, through
# run-tool-tests.sh, after its embedded build/release; off macOS, without an embedded build, or for another
# platform, it reports SKIP, but an embedded osx_amd64 or osx_arm64 build without the Mach-O object fails.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$here/../../../build/release"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}
# stop <line>: reports one more case, "ok" (a SKIP) or "not ok", and ends with the suite's result.
stop() {
  n=$((n+1)); echo "${1/\#/$n -}"
  [[ "$1" == "ok #"* ]] || failed=$((failed+1))
  echo "# $n cases, $((n - failed)) passed"; exit $(( failed > 0 ))
}

[[ "$(uname -s)" == Darwin ]] || stop "ok # macOS embed checks # SKIP this host is not macOS"
cached() { sed -n "s/^$1=//p" "$build/CMakeCache.txt" 2>/dev/null | tr -d '\r'; }
if [[ "$(cached REDSHIFT_CLIENT:STRING)" != odbc || "$(cached REDSHIFT_DRIVER:STRING)" != embedded ]]; then
  stop "ok # macOS embed checks # SKIP build/release is not a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=embedded build"
fi
platform="$(sed -n 's/^platform //p' "$build/extension/redshift/redshift_driver_identity.txt" 2>/dev/null | tr -d '\r')" || true
[[ -n "$platform" ]] || stop "not ok # an embedded build records its platform in redshift_driver_identity.txt"
[[ "$platform" == osx_amd64 || "$platform" == osx_arm64 ]] ||
  stop "ok # macOS embed checks # SKIP build/release is for $platform, which has no Mach-O embed"
lib="$build/extension/redshift/libredshift_extension.a"
loadable="$build/extension/redshift/redshift.duckdb_extension"
header="$build/extension/redshift/redshift_driver/driver_info.h"
if [[ ! -f "$header" ]] || ! ar t "$lib" 2>/dev/null | grep -qx embedded_driver_macho.S.o; then
  stop "not ok # an embedded macOS build has driver_info.h and the Mach-O embed object"
fi
nm=/usr/bin/nm otool=/usr/bin/otool
ar p "$lib" embedded_driver_macho.S.o > "$tmp/embed.o"
stems="$(sed -n 's/^[[:space:]]*X("[^"]*", [0-9]*, "[0-9a-f]*", \([a-z0-9_]*\)).*/\1/p' "$header")"

symbols_are_private_const() {
  local s edge listing
  listing="$("$nm" -m "$tmp/embed.o")" || return 1
  [[ -n "$stems" ]] || return 1
  for s in $stems; do
    for edge in begin end; do
      grep -qE "\(__TEXT,__const\) private external _redshift_driver_${s}_${edge}\$" <<<"$listing" ||
        { echo "# $s $edge: $(grep "_redshift_driver_${s}_${edge}\$" <<<"$listing")" >&2; return 1; }
    done
  done
}
# The object's header flags (no MH_SUBSECTIONS_VIA_SYMBOLS) and its __const section: aligned to 2^6, with no
# attribute that marks instructions. otool -hv names the flags; the header line is the one with MH_MAGIC_64.
no_subsections() {
  local header
  header="$("$otool" -hv "$tmp/embed.o" | grep MH_MAGIC_64)" || return 1
  [[ "$header" != *SUBSECTIONS_VIA_SYMBOLS* ]]
}
const_section() { "$otool" -l "$tmp/embed.o" | awk '/sectname __const/ {f=1} f && $1 == "'"$1"'" {print $2; exit}'; }
const_is_aligned_data() { [[ "$(const_section align)" == "2^6" && "$(const_section flags)" == 0x00000000 ]]; }

# Each range of the object's __const, by its symbols' values, against driver_info.h's rows; and the
# loadable's copy of each file, found by its bytes.
python_check() {
  python3 -I - "$header" "$1" "$2" <<'PY'
import codecs, hashlib, re, struct, sys
header, path, mode = sys.argv[1:]
rows = re.findall(r'X\("([^"]+)", (\d+), "([0-9a-f]{64})", (\w+)\)', open(header).read())
data = open(path, "rb").read()
if mode == "loadable":
    paths = dict(re.findall(r'X\((\w+), "([^"]+)"\)', open(header).read()))
    bad = []
    for name, size, sha, stem in rows:
        # The header writes bytes outside [A-Za-z0-9/._+-] as octal escapes, which this reads back.
        want = open(codecs.escape_decode(paths[stem].encode())[0], "rb").read()
        if hashlib.sha256(want).hexdigest() != sha or data.find(want) < 0:
            bad.append(name)
    print("# rows %d, not found in the loadable extension: %s" % (len(rows), ", ".join(bad) or "none"))
    sys.exit(1 if bad or not rows else 0)
magic, cpu, sub, ftype, ncmds = struct.unpack_from("<IiiII", data, 0)
if magic != 0xFEEDFACF:
    print("# not a 64-bit little-endian Mach-O object (magic 0x%08x)" % magic)
    sys.exit(1)
off, sect, syms = 32, None, {}
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", data, off)
    if cmd == 0x19:
        for i in range(struct.unpack_from("<I", data, off + 64)[0]):
            s = off + 72 + 80 * i
            if data[s:s + 16].rstrip(b"\0") == b"__const":
                addr, sz, fileoff = struct.unpack_from("<QQI", data, s + 32)
                sect = (addr, sz, fileoff)
    elif cmd == 0x2:
        symoff, nsyms, stroff = struct.unpack_from("<III", data, off + 8)
        for i in range(nsyms):
            strx, _, _, _, value = struct.unpack_from("<IBBhQ", data, symoff + 16 * i)
            syms[data[stroff + strx:data.index(b"\0", stroff + strx)].decode()] = value
    off += size
bad = []
for name, size, sha, stem in rows:
    b, e = syms["_redshift_driver_%s_begin" % stem], syms["_redshift_driver_%s_end" % stem]
    chunk = data[sect[2] + b - sect[0]:sect[2] + e - sect[0]]
    if e - b != int(size) or hashlib.sha256(chunk).hexdigest() != sha or e >= sect[0] + sect[1]:
        bad.append(name)
print("# rows %d, mismatched: %s" % (len(rows), ", ".join(bad) or "none"))
sys.exit(1 if bad or not rows or sect is None else 0)
PY
}
embedded_bytes_match_the_lock() { python_check "$tmp/embed.o" object; }
loadable_carries_the_set() { python_check "$loadable" loadable; }
exported() { "$nm" -g -U "$loadable"; }
loadable_exports_no_driver_symbol() { local e; e="$(exported)" && ! grep -qE 'redshift_driver|LoadEmbeddedDriver|DriverLoader' <<<"$e"; }
loadable_exports_one_function() { [[ "$(exported | awk '$2 == "T" {print $3}')" == _redshift_duckdb_cpp_init ]]; }

check "every file of the set has a private external begin and end symbol in __TEXT,__const" symbols_are_private_const
check "the object has no .subsections_via_symbols, so the linker keeps the section whole" no_subsections
check "__TEXT,__const is data only and 64-byte aligned" const_is_aligned_data
check "each embedded range has the size and SHA-256 driver_info.h records, and no end symbol ends the section" \
  embedded_bytes_match_the_lock
check "the loadable extension carries every file of the set (-dead_strip kept them)" loadable_carries_the_set
check "the loadable extension exports no driver symbol" loadable_exports_no_driver_symbol
check "the embed adds no exported function: the only one (type T) is DuckDB's entry point" loadable_exports_one_function

# sources.cmake's compile check, in a scratch project that includes it as embed-build-graph_test.sh's does: the
# platform's own architecture configures; a universal build and the other architecture are refused, with the hint
# to build the platform's one architecture or set another DUCKDB_PLATFORM, also in a build directory that passed
# before, where DuckDB rewrites CMAKE_OSX_ARCHITECTURES (the cached result is keyed by it). Configures only: nothing
# is fetched.
case "$platform" in osx_arm64) own=arm64 other=x86_64 ;; *) own=x86_64 other=arm64 ;; esac
proj="$tmp/compile-check"
mkdir -p "$proj"
ln -s "$here/../../../src" "$proj/src"; ln -s "$here/../../../third_party" "$proj/third_party"
printf '%s\n' 'cmake_minimum_required(VERSION 3.19)' 'project(compile_check C)' 'set(REDSHIFT_DRIVER embedded)' \
  'include(src/transport/driver/sources.cmake)' > "$proj/CMakeLists.txt"
refusal="the platform is $platform, but the C compiler does not target 64-bit macOS"
hint="build one architecture, with OSX_BUILD_ARCH=$own, or set DUCKDB_PLATFORM to the target's DuckDB platform"
configure_for() {  # configure_for <build dir> <CMAKE_OSX_ARCHITECTURES>: prints the output, fails as cmake does
  env -u RS_DRIVER_LOCK -u RS_DRIVER_MIRROR -u MAKEFLAGS -u CMAKE_GENERATOR cmake -G "Unix Makefiles" -S "$proj" \
    -B "$1" "-DDUCKDB_EXPLICIT_PLATFORM=$platform" "-DCMAKE_OSX_ARCHITECTURES=$2" 2>&1
}
configures() {  # configures <build dir> <architectures>
  local out
  out="$(configure_for "$1" "$2")" || { echo "# $2: $(tail -3 <<<"$out")" >&2; return 1; }
}
refused_for() {  # refused_for <build dir> <architectures>
  local out flat
  out="$(configure_for "$1" "$2")" && return 1
  flat="$(tr -s ' \n' ' ' <<<"$out")"
  [[ "$flat" == *"$refusal"* && "$flat" == *"$hint"* ]] || { echo "# $2: $(tail -3 <<<"$out")" >&2; return 1; }
}
check "the compile check passes $platform built for $own alone" configures "$tmp/check-$own" "$own"
check "the compile check refuses a universal build ($own and $other) for $platform" \
  refused_for "$tmp/check-universal" "$own;$other"
check "the compile check refuses $platform built for $other" refused_for "$tmp/check-other" "$other"
check "the compile check refuses $other in a build directory that passed $own (the result is keyed by the architectures)" \
  refused_for "$tmp/check-$own" "$other"
echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
