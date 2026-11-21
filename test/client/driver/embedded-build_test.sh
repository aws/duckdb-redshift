#!/usr/bin/env bash
# Checks the embed: the assembler object holds a hidden begin and end symbol per file of the set in
# read-only data, a non-executable stack note and a GNU property note that keeps a hardened link's
# marking; each pair's bytes have the lock's SHA-256; and the loadable extension exports nothing
# more. In the loadable extension itself: a non-executable stack, and each file of the set with the
# lock's SHA-256 in a read-only segment, two rules that also fail on stand-ins linked to break them.
# The note is first checked without a build. make test_driver runs it, through run-tool-tests.sh,
# after its embedded build/release; without one, or for a platform with no ELF embed, the build's
# cases SKIP, but an embedded linux_amd64 or linux_arm64 build without the embed fails.
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

# The GNU property note readelf -n shows for an object of each architecture: GNU ld keeps a
# hardened link's marking only when every input declares it, and the embed holds no code. readelf
# from binutils 2.42 and earlier prints aarch64's GCS bit as "<unknown: 4>".
feature() {
  case "$1" in x86_64) echo "x86 feature: IBT, SHSTK" ;; aarch64) echo "AArch64 feature: BTI, PAC, (GCS|<unknown: 4>)" ;; esac
}
features() { case "$1" in x86_64) echo "IBT, SHSTK" ;; aarch64) echo "BTI, PAC, GCS" ;; esac; }
# feature_line <object>: the features its property notes declare ("x86 feature: ..." or "AArch64
# feature: ..."), or nothing; it fails on any readelf warning, such as a note whose sizes are wrong.
# Without -W, readelf prints one property per line, so the line holds only the features.
feature_line() {
  local text; text="$(readelf -n "$1" 2>&1)" || return 1
  [[ "$text" != *Warning* ]] || { echo "# readelf -n $1: $(printf '%s' "$text" | grep Warning | head -1)" >&2; return 1; }
  printf '%s\n' "$text" | grep -oE '(x86|AArch64) feature: .*' || true
}
# declares_features <object> <arch>: one line, exactly the architecture's features, in a
# .note.gnu.property section aligned to 8 bytes, as ELF64 requires.
declares_features() {
  local line align
  line="$(feature_line "$1")" && [[ "$line" =~ ^$(feature "$2")$ ]] || return 1
  align="$(readelf -SW "$1" | awk '/\] \.note\.gnu\.property / {print $NF}')"
  [[ "$align" == 8 ]]
}
# Without a build: embedded_driver_elf.S assembled with a stand-in driver_info.h of one file, by
# the host's C compiler for its own architecture and by clang for either, where they are present.
note_dir="$tmp/note"; mkdir -p "$note_dir"; printf 'x' > "$note_dir/file"
printf '#define REDSHIFT_DRIVER_EMBED_FILES(X) X(file, "%s")\n' "$note_dir/file" > "$note_dir/driver_info.h"
clang_cmd="$(command -v clang || compgen -c clang- | grep -E '^clang-[0-9]+$' | sort -t- -k2 -n | tail -1 || true)"
assembles_with_note() {  # assembles_with_note <arch> <compiler...>
  local arch="$1"; shift
  "$@" -I "$note_dir" -c "$here/../../../src/transport/driver/embedded_driver_elf.S" -o "$note_dir/$arch.o" &&
    declares_features "$note_dir/$arch.o" "$arch" || return 1
  # On aarch64 the note is the section's only one, so a note aligned to 4 gives a section aligned
  # to 4, which must be refused (on x86-64, gas's own note keeps the section at 8).
  [[ "$arch" == aarch64 ]] || return 0
  sed '/^\t\.section \.note\.gnu\.property/{n;s/\.p2align 3/.p2align 2/;}' \
    "$here/../../../src/transport/driver/embedded_driver_elf.S" > "$note_dir/align4.S" &&
    "$@" -I "$note_dir" -c "$note_dir/align4.S" -o "$note_dir/align4.o" &&
    [[ "$(readelf -SW "$note_dir/align4.o" | awk '/\] \.note\.gnu\.property / {print $NF}')" == 4 ]] &&
    ! declares_features "$note_dir/align4.o" "$arch"
}
for arch in x86_64 aarch64; do
  name="assembled for $arch, the embed declares $(features "$arch") in its GNU property note"
  [[ "$arch" == x86_64 ]] || name+=", aligned to 8 (a note aligned to 4 is refused)"
  if ! command -v readelf >/dev/null; then echo "ok $((n += 1)) - $name # SKIP readelf is not on PATH"
  elif [[ "$(uname -s)/$(uname -m)" == "Linux/$arch" ]] && command -v "${CC:-cc}" >/dev/null; then
    check "$name (${CC:-cc})" assembles_with_note "$arch" "${CC:-cc}"
  elif [[ -n "$clang_cmd" ]]; then
    check "$name ($(basename "$clang_cmd"))" assembles_with_note "$arch" "$clang_cmd" "--target=$arch-linux-gnu"
  else echo "ok $((n += 1)) - $name # SKIP no assembler for $arch: the host is $(uname -m) and clang is not on PATH"; fi
done
# The checks above see a malformed note: without its padding, the property runs into the next
# note (gas adds its own on x86-64) or past the section, and readelf warns.
malformed_note_is_refused() {
  sed '/padding to 8 bytes/d' "$here/../../../src/transport/driver/embedded_driver_elf.S" > "$note_dir/unpadded.S" &&
    "${CC:-cc}" -I "$note_dir" -c "$note_dir/unpadded.S" -o "$note_dir/unpadded.o" &&
    ! grep -q 'padding to 8 bytes' "$note_dir/unpadded.S" && ! declares_features "$note_dir/unpadded.o" "$(uname -m)" 2>/dev/null
}
name="a property note without its padding, which readelf warns about, is refused"
if command -v readelf >/dev/null && [[ "$(uname -s)/$(uname -m)" =~ ^Linux/(x86_64|aarch64)$ ]] && command -v "${CC:-cc}" >/dev/null; then
  check "$name" malformed_note_is_refused
else echo "ok $((n += 1)) - $name # SKIP it needs readelf and a C compiler for a Linux x86_64 or aarch64 host"; fi

cached() { sed -n "s/^$1=//p" "$build/CMakeCache.txt" 2>/dev/null | tr -d '\r'; }
if [[ "$(cached REDSHIFT_CLIENT:STRING)" != odbc || "$(cached REDSHIFT_DRIVER:STRING)" != embedded ]]; then
  stop "ok # embedded build checks # SKIP build/release is not a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=embedded build"
fi
# The platform the configure chose, as src/transport/driver/sources.cmake records it for every
# embedded build; linux_amd64 and linux_arm64 embed ELF (macOS's Mach-O embed is embedded-macho_test.sh's).
platform="$(sed -n 's/^platform //p' "$build/extension/redshift/redshift_driver_identity.txt" 2>/dev/null | tr -d '\r')" || true
if [[ -z "$platform" ]]; then
  stop "not ok # an embedded build records its platform in redshift_driver_identity.txt"
fi
if [[ "$platform" != linux_amd64 && "$platform" != linux_arm64 ]]; then
  stop "ok # embedded build checks # SKIP build/release is for $platform, which has no ELF embed"
fi
if [[ ! -f "$build/extension/redshift/redshift_driver/driver_info.h" ]] ||
  ! ar t "$build/extension/redshift/libredshift_extension.a" 2>/dev/null | grep -qx embedded_driver_elf.S.o; then
  stop "not ok # an embedded Linux build has driver_info.h and the embed object"
fi
lib="$build/extension/redshift/libredshift_extension.a"
loadable="$build/extension/redshift/redshift.duckdb_extension"
ar p "$lib" embedded_driver_elf.S.o > "$tmp/embed.o"
# Each X(name, size, sha256, stem) row of driver_info.h names one file's symbol stem.
stems="$(sed -n 's/^[[:space:]]*X("[^"]*", [0-9]*, "[0-9a-f]*", \([a-z0-9_]*\)).*/\1/p' \
  "$build/extension/redshift/redshift_driver/driver_info.h")"

# section <name> <field>: a field of readelf -SW's line for section <name>, counted after the
# "[ n]" index: 0 the index, 1 the name, 2 the type, ... 7 the flags (the next column when empty).
section() {
  readelf -SW "$tmp/embed.o" | awk -v s="$1" -v f="$2" '
    match($0, /^ *\[ *[0-9]+\]/) { i = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", i)
      rest = substr($0, RSTART + RLENGTH); split(rest, a, " "); a[0] = i; if (a[1] == s) print a[f] }'
}
symbols_are_hidden_rodata() {
  local s sect
  sect="$(section .rodata 0)"
  [[ -n "$stems" && -n "$sect" ]] || return 1
  for s in $stems; do
    for edge in begin end; do
      readelf -sW "$tmp/embed.o" | awk -v n="redshift_driver_${s}_$edge" -v x="$sect" \
        '$8 == n && $5 == "GLOBAL" && $6 == "HIDDEN" && $7 == x {f=1} END {exit !f}' || return 1
    done
  done
}
rodata_is_read_only() { [[ "$(section .rodata 7)" == A ]]; }
stack_note_is_not_executable() { local t; t="$(section .note.GNU-stack 2)"; [[ "$t" == PROGBITS && "$(section .note.GNU-stack 7)" != *X* ]]; }
arch=x86_64 hardening=-fcf-protection=full
[[ "$platform" == linux_amd64 ]] || arch=aarch64 hardening=-mbranch-protection=standard
build_object_declares_features() { declares_features "$tmp/embed.o" "$arch"; }
# Linked with an object the build's C compiler hardens, into a shared object as the loadable
# extension is, the embed keeps every feature that object declares, whichever the compiler sets.
hardened_link_keeps_marking() {
  local o_line so_line
  printf 'int redshift_hardened(void) { return 0; }\n' > "$tmp/hardened.c"
  "$(cached CMAKE_C_COMPILER:FILEPATH)" "$hardening" -fPIC -c "$tmp/hardened.c" -o "$tmp/hardened.o" &&
    "$(cached CMAKE_C_COMPILER:FILEPATH)" -shared -nostdlib "$tmp/hardened.o" "$tmp/embed.o" -o "$tmp/hardened.so" &&
    o_line="$(feature_line "$tmp/hardened.o")" && so_line="$(feature_line "$tmp/hardened.so")" || return 1
  echo "# $hardening object: ${o_line:-no features}; linked with the embed: ${so_line:-no features}"
  [[ -n "$o_line" && "$so_line" == "$o_line" ]]
}
loadable_exports_no_driver_symbol() { ! nm -D --defined-only "$loadable" | grep -q redshift_driver; }
loadable_exports_one_function() {
  [[ "$(nm -D --defined-only "$loadable" | awk '$2 == "T" {print $3}')" == redshift_duckdb_cpp_init ]]
}

# Each begin..end range of .rodata, hashed, against the X(name, size, sha256, stem) rows.
embedded_bytes_match_the_lock() {
  objcopy -O binary --only-section=.rodata "$tmp/embed.o" "$tmp/rodata.bin" || return 1
  readelf -sW "$tmp/embed.o" > "$tmp/symbols.txt"
  python3 -I - "$build/extension/redshift/redshift_driver/driver_info.h" "$tmp/symbols.txt" "$tmp/rodata.bin" <<'PY'
import hashlib, re, sys
header, symbols, rodata = sys.argv[1:]
rows = re.findall(r'X\("([^"]+)", (\d+), "([0-9a-f]{64})", (\w+)\)', open(header).read())
value = {f[7]: int(f[1], 16) for f in (l.split() for l in open(symbols)) if len(f) == 8 and f[0].rstrip(":").isdigit()}
data = open(rodata, "rb").read()
bad = [name for name, size, sha, stem in rows
       if hashlib.sha256(data[value["redshift_driver_%s_begin" % stem]:value["redshift_driver_%s_end" % stem]]).hexdigest() != sha
       or value["redshift_driver_%s_end" % stem] - value["redshift_driver_%s_begin" % stem] != int(size)]
print("# rows %d, mismatched: %s" % (len(rows), ", ".join(bad) or "none"))
sys.exit(1 if bad or not rows else 0)
PY
}

check "every file of the set has a hidden begin and end symbol in .rodata" symbols_are_hidden_rodata
check "each embedded range has the size and SHA-256 driver_info.h records from the lock" embedded_bytes_match_the_lock
check "the embedded data is read-only, not writable or executable" rodata_is_read_only
check "the embed object carries a non-executable .note.GNU-stack" stack_note_is_not_executable
check "the embed object declares $(features "$arch") in its GNU property note" build_object_declares_features
check "linked with a $hardening object into a shared object, the embed keeps its $(features "$arch") marking" \
  hardened_link_keeps_marking
check "the loadable extension exports no driver symbol" loadable_exports_no_driver_symbol
check "the embed adds no exported function: the only one (type T) is DuckDB's entry point" loadable_exports_one_function
# The file DuckDB loads: its stack is not executable, and each file of the set is in it with the
# lock's size and SHA-256, inside one loaded segment that is not writable. That segment may be executable:
# GNU ld on aarch64 puts .rodata in the text segment (R E), so read-only is all the rule asks. Each takes
# the shared object to check, the loadable extension by default.
loadable_stack_is_not_executable() {
  [[ "$(readelf -lW "${1:-$loadable}" | awk '$1 == "GNU_STACK" {print $7}')" == RW ]]
}
loadable_holds_the_set_read_only() {
  local object="${1:-$loadable}"
  readelf -lW "$object" > "$tmp/loadable-segments.txt" && readelf -sW "$object" > "$tmp/loadable-symbols.txt" || return 1
  python3 -I - "$build/extension/redshift/redshift_driver/driver_info.h" "$tmp/loadable-segments.txt" \
    "$tmp/loadable-symbols.txt" "$object" <<'PY'
import hashlib, re, sys
header, segments, symbols, path = sys.argv[1:]
rows = re.findall(r'X\("([^"]+)", (\d+), "([0-9a-f]{64})", (\w+)\)', open(header).read())
# LOAD <offset> <vaddr> <paddr> <filesz> <memsz> <flags...> <align>
loads = [(int(f[1], 16), int(f[2], 16), int(f[4], 16), " ".join(f[6:-1])) for f in (l.split() for l in open(segments)) if f[:1] == ["LOAD"]]
value = {f[7]: int(f[1], 16) for f in (l.split() for l in open(symbols)) if len(f) == 8 and f[0].rstrip(":").isdigit()}
data = open(path, "rb").read()
bad = []
for name, size, sha, stem in rows:
    begin, end = value.get("redshift_driver_%s_begin" % stem), value.get("redshift_driver_%s_end" % stem)
    owner = [s for s in loads if begin is not None and end is not None and s[1] <= begin and end <= s[1] + s[2]]
    if len(owner) != 1 or "W" in owner[0][3]:
        bad.append(name + " (segment)")
        continue
    offset, vaddr = owner[0][0], owner[0][1]
    blob = data[offset + begin - vaddr:offset + end - vaddr]
    if len(blob) != int(size) or hashlib.sha256(blob).hexdigest() != sha:
        bad.append(name + " (bytes)")
print("# rows %d, mismatched: %s" % (len(rows), ", ".join(bad) or "none"))
sys.exit(1 if bad or not rows else 0)
PY
}
check "the loadable extension's stack (PT_GNU_STACK) is not executable" loadable_stack_is_not_executable
check "the loadable extension holds each file of the set with the lock's SHA-256, in a read-only segment" \
  loadable_holds_the_set_read_only
# The two rules on shared objects linked from the embed: as it is, which passes both; with an executable
# stack (-z execstack); and with its .rodata made writable, so the linker puts it in a writable segment.
# Each broken one must fail its rule.
stand_in() {  # stand_in <name> <object> <link flag...>: links <object> alone into $tmp/<name>.so
  local name="$1" object="$2"; shift 2
  "$(cached CMAKE_C_COMPILER:FILEPATH)" -shared -nostdlib "$@" "$object" -o "$tmp/$name.so"
}
not() { ! "$@"; }
if stand_in plain "$tmp/embed.o" -Wl,-z,noexecstack && stand_in execstack "$tmp/embed.o" -Wl,-z,execstack &&
  objcopy --set-section-flags .rodata=alloc,load,contents,data "$tmp/embed.o" "$tmp/writable.o" &&
  stand_in writable "$tmp/writable.o" -Wl,-z,noexecstack; then
  check "the stack rule passes the embed linked as it is" loadable_stack_is_not_executable "$tmp/plain.so"
  check "the stack rule fails the embed linked with -z execstack" not loadable_stack_is_not_executable "$tmp/execstack.so"
  check "the read-only rule passes the embed linked as it is" loadable_holds_the_set_read_only "$tmp/plain.so"
  check "the read-only rule fails the embed with a writable .rodata" not loadable_holds_the_set_read_only "$tmp/writable.so"
else
  check "the compiler links the embed into the stand-in shared objects" false
fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
