#!/usr/bin/env bash
# Tool tests for the macOS loading policy: lib/macho.py policy on each slice of the
# universal library, and lib/env_names.py's rule on each slice macho.py thin writes out,
# as verify_macos.sh runs them. The tool reads only the forms 2.3.0.0 uses and refuses every other
# by name. The unmodified locked library passes the policy; copies of it with one field changed by
# patch_macho.py, and tiny libraries make_macho.py writes from byte templates, each fail at their
# rule. Run by run-tool-tests.sh.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=macos-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/macos-test-lib.sh"

# policy <dylib> [<policy-dir>]: the loading policy, the repository's unless another is given.
policy() { out_text="$(python3 -I "$lib/macho.py" policy "$1" "${2:-$odbc/policy/macos}" "$header" 2>&1)" && status=0 || status=$?; }
# envrule <dylib> [<policy-dir>]: the environment-name rule on each slice as its own platform, as
# verify_macos.sh runs it; out_text holds each failing slice's problems, each line led by the
# slice, or, when both pass, both summaries.
envrule() {
  local slice msg failed="" passed=""
  rm -rf -- "${tmp:?}/slices"; mkdir "$tmp/slices"
  python3 -I "$lib/macho.py" thin "$1" "$tmp/slices"
  for slice in x86_64:osx_amd64 arm64:osx_arm64; do
    if msg="$(python3 -I "$lib/env_names.py" "${2:-$odbc/policy}" "$tmp/slices/${slice%:*}" "${slice#*:}" 2>&1)"; then
      passed+="${passed:+$'\n'}${slice%:*}: $msg"
    else failed+="${failed:+$'\n'}$(sed "s/^/${slice%:*}: /" <<<"$msg")"; fi
  done
  if [[ -n "$failed" ]]; then out_text="$failed" status=1; else out_text="$passed" status=0; fi
}
patch() { python3 -I "$here/patch_macho.py" "$@"; }

real="$(real_package)"
if [[ -n "$real" ]]; then
  python3 -I "$lib/xar.py" extract "$real" "$mac_path" "$tmp/real.dylib" sha1
  r="$tmp/real.dylib" p="$tmp/p.dylib"
  policy "$r"
  expect "the unmodified locked library fits the macOS policy, and the summary counts its weak binds" 0 \
    "the library fits the macOS policy (minimum macOS 11.7.0, newer than DuckDB's 11.0.0, as min-os.txt accepts); by weak lookup, which dyld resolves by name across the process, it binds 2,211 names in x86_64 and 1,293 in arm64"
  # expected-exports.txt is what macho.py exports prints for the locked library, row for row.
  python3 -I "$lib/macho.py" exports "$r" > "$tmp/generated.txt"
  if awk '!/^#/ && NF' "$odbc/policy/macos/expected-exports.txt" | python3 -I -c 'import sys
sys.exit(sys.stdin.read() != open(sys.argv[1]).read())' "$tmp/generated.txt"; then
    pass "macho.py exports reproduces expected-exports.txt's 14,486 rows from the locked library"
  else fail "macho.py exports reproduces expected-exports.txt ($(($(wc -l < "$tmp/generated.txt"))) rows generated)"; fi # BSD wc pads its count
  patch "$r" "$p" dylib x86_64 /usr/lib/libother.dylib; policy "$p"
  expect "a changed linked-library path fails" 1 "x86_64: linked-libraries: LC_LOAD_DYLIB loads /usr/lib/libother.dylib, which is not in allowed-load-dylibs.txt"
  patch "$r" "$p" rpath arm64 @loader_path/lib; policy "$p"
  expect "an LC_RPATH added in spare header space fails" 1 "arm64: search-path: the slice has an LC_RPATH"
  patch "$r" "$p" minos x86_64 12.0.0; policy "$p"
  expect "a raised minimum macOS fails" 1 "x86_64: min-os: the slice needs macOS 12.0.0, newer than 11.7.0 in min-os.txt"
  patch "$r" "$p" dylib-offset arm64 4096; policy "$p"
  expect "a library name outside its load command fails" 1 "arm64: linked-libraries: a load command's library name lies outside it"
  patch "$r" "$p" platform x86_64 2; policy "$p"
  expect "a minimum version for another platform than macOS fails" 1 "x86_64: min-os: the slice does not name exactly one minimum macOS version"
  patch "$r" "$p" flags arm64 0x918105; policy "$p"
  expect "the locked library with a flat namespace forced in arm64 fails" 1 "arm64: namespace: the slice's flags 0x918105 do not name two-level namespaces"
  # One bind through a flat lookup, or the host, in a two-level image (as ld's -undefined
  # dynamic_lookup writes): the header flags still pass, the ordinal does not.
  patch "$r" "$p" bind-ordinal x86_64 bind:-2; policy "$p"
  expect "the locked library with one x86_64 bind through a flat lookup fails" 1 \
    "x86_64: namespace: a bind opcode names a flat lookup (ordinal -2), not one of the slice's libraries (1 to 8)"
  patch "$r" "$p" bind-ordinal arm64 lazy:-1; policy "$p"
  expect "the locked library with one arm64 lazy bind to the host executable fails" 1 \
    "arm64: namespace: a lazy-bind opcode names the host executable (ordinal -1), not one of the slice's libraries (1 to 8)"
  # A weak bind must be of a name the slice exports or also binds by ordinal: a renamed weak-bind
  # name of an export (x86_64) or of operator new, which libc++ defines (arm64), is neither.
  patch "$r" "$p" weak-name x86_64 __Z15safeNumericCastIadET_T0_Pb; policy "$p"
  expect "the locked library with an x86_64 weak bind of a name it neither exports nor binds by ordinal fails" 1 \
    "x86_64: weak-binds: the slice binds names by weak lookup that it neither exports nor binds by ordinal to one of its libraries, so dyld would find them by name alone: __Z15safeNumericCastIadET_T0_PY"
  patch "$r" "$p" weak-name arm64 __Znwm; policy "$p"
  expect "the locked library with operator new's arm64 weak bind renamed fails" 1 "arm64: weak-binds: the slice binds names by weak lookup that it neither exports nor binds by ordinal to one of its libraries, so dyld would find them by name alone: __ZnwY"
  patch "$r" "$p" weak-flag x86_64 __Znwm; policy "$p"
  expect "the locked library with a weak-bind entry flagged as its own non-weak definition fails" 1 \
    "x86_64: weak-binds: the weak-bind opcodes flag names as the slice's own non-weak definitions, which would take the place of every other image's weak definition of them: __Znwm"
  patch "$r" "$p" export arm64 _SQLGetDiagFieldW; policy "$p"
  expect "a renamed export fails, naming both names" 1 "arm64: exports: the slice's exports differ from expected-exports.txt; extra: SQLGetDiagFieldY; missing: SQLGetDiagFieldW"
  # The forms the tool does not read, put into the locked library: each fails at load-commands, by
  # name, before any bind is read; so does the one LC_DYLD_INFO_ONLY retagged as LC_DYLD_INFO.
  for c in "0x80000034:chained fixups (LC_DYLD_CHAINED_FIXUPS)" "0x80000033:a separate export trie (LC_DYLD_EXPORTS_TRIE)" \
    "0x24:LC_VERSION_MIN_MACOSX"; do
    patch "$r" "$p" add-command x86_64 "${c%%:*}"; policy "$p"
    expect "the locked library with ${c#*:} added fails at load-commands" 1 \
      "x86_64: load-commands: the slice uses ${c#*:}, which this tool does not read; a reviewed change adds it"
  done
  patch "$r" "$p" retag arm64 0x80000022:0x22; policy "$p"
  expect "the locked library with LC_DYLD_INFO in place of LC_DYLD_INFO_ONLY fails at load-commands" 1 \
    "arm64: load-commands: the slice uses LC_DYLD_INFO, not LC_DYLD_INFO_ONLY, which this tool does not read; a reviewed change adds it"
  envrule "$r"
  expect "the unmodified locked library's environment-variable names are all classified, in both slices" 0 \
    "x86_64: the driver's 756 scanned names are all classified"
  [[ "$out_text" == *"arm64: the driver's 756 scanned names are all classified"* ]] &&
    pass "the arm64 slice is checked on its own" || fail "the arm64 slice is checked on its own ($out_text)"
else skip "the locked library's loading policy" "$no_real"; fi

# Tiny libraries, against a policy that lists what they hold: the interface's entry points as
# exports, libSystem, and the repository's minimum macOS rows.
tp="$tmp/policy/macos"; mkdir -p "$tp"
cp "$odbc/policy/macos/allowed-load-dylibs.txt" "$odbc/policy/macos/min-os.txt" "$tp/"
entry_points > "$tp/expected-exports.txt"
exports="$(entry_points_json)"
tiny() { python3 -I "$here/make_macho.py" "$tmp/tiny.dylib" "{\"exports\": ${2:-$exports}, $1}"; policy "$tmp/tiny.dylib" "$tp"; }
tiny '"minos": "11.0"'
expect "a tiny library that holds what the policy lists passes" 0 "the library fits the macOS policy (minimum macOS 11.0.0)"
[[ "$out_text" == "the library fits the macOS policy (minimum macOS 11.0.0)" ]] &&
  pass "a library without weak binds prints no weak-bind count" || fail "a library without weak binds prints no weak-bind count ($out_text)"
out_text="$(python3 -I "$lib/macho.py" exports "$tmp/tiny.dylib" 2>&1)" && status=0 || status=$?
[[ "$status" == 0 && "$out_text" == "$(entry_points | LC_ALL=C sort)" ]] &&
  pass "macho.py exports prints a tiny library's exports, both slices' rows bare" || fail "macho.py exports on a tiny library (exit $status; $out_text)"
# expected-exports.txt lists names without their leading underscore, so every export must have one:
# then each row is exactly one name of the trie. An export without one (foo, 666f6f) fails at
# exports, alone and beside its twin _foo (5f666f6f).
bare_why="exports: the slice exports names without a leading underscore, which expected-exports.txt, listing names without it, cannot tell from the same names with one"
for c in "alone:0001666f6f000702000000" "beside _foo:00025f666f6f000d666f6f00110200000002000000"; do
  python3 -I "$here/make_macho.py" "$tmp/bare.dylib" "{\"trie_hex\": \"${c#*:}\"}"
  out_text="$(python3 -I "$lib/macho.py" exports "$tmp/bare.dylib" 2>&1)" && status=0 || status=$?
  expect "macho.py exports fails on an export without a leading underscore, ${c%%:*}, naming it" 1 "x86_64: $bare_why: foo"
done
many_bare="$(python3 -I -B -c 'import importlib.util, sys
spec = importlib.util.spec_from_file_location("make_macho", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
print(m.trie([chr(c) for c in range(0x75, 0x60, -1)]).hex())' "$here/make_macho.py")"
python3 -I "$here/make_macho.py" "$tmp/bare.dylib" "{\"trie_hex\": \"$many_bare\"}"
out_text="$(python3 -I "$lib/macho.py" exports "$tmp/bare.dylib" 2>&1)" && status=0 || status=$?
expect "21 exports without a leading underscore fail, the first 20 named, sorted" 1 "x86_64: $bare_why: a b c d e f g h i j k l m n o p q r s t ..."
# The policy too, in a library that also weak-binds the bare name: the trie holds the interface's
# entry points and SQLFetch beside _SQLFetch, which expected-exports.txt's one row SQLFetch would
# otherwise stand for both of.
twin="$(entry_points | python3 -I -B -c 'import importlib.util, sys
spec = importlib.util.spec_from_file_location("make_macho", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
print(m.trie(["_" + n for n in sys.stdin.read().split()] + ["SQLFetch"]).hex())' "$here/make_macho.py")"
tiny "\"minos\": \"11.0\", \"trie_hex\": \"$twin\", \"weak_bind_hex\": \"4053514c46657463680090\""
expect "an export without a leading underscore beside its twin fails at exports, though weak-bound" 1 "x86_64: $bare_why: SQLFetch"
python3 -I "$here/make_macho.py" "$tmp/one.dylib" '{"exports": ["SQLFetch"], "archs": ["x86_64"]}'
out_text="$(python3 -I "$lib/macho.py" exports "$tmp/one.dylib" 2>&1)" && status=0 || status=$?
expect "macho.py exports on a library without both slices fails" 1 "slices: the library has 1 slices"
# The forms 2.3.0.0 does not use fail at load-commands, by name, before any other rule; each is an
# 8-byte command here, as the tool reads no field of it.
not_read="which this tool does not read; a reviewed change adds it"
for c in "$((0x80000034)):chained fixups (LC_DYLD_CHAINED_FIXUPS)" "$((0x80000033)):a separate export trie (LC_DYLD_EXPORTS_TRIE)" \
  "$((0x24)):LC_VERSION_MIN_MACOSX" "$((0x22)):LC_DYLD_INFO, not LC_DYLD_INFO_ONLY"; do
  tiny "\"minos\": \"11.0\", \"command\": ${c%%:*}"
  expect "a slice with ${c#*:} fails at load-commands, naming it" 1 "x86_64: load-commands: the slice uses ${c#*:}, $not_read"
done
tiny '"minos": "11.0", "trie": "old-info"'
expect "a slice whose binds and export trie are in LC_DYLD_INFO alone fails at load-commands" 1 \
  "x86_64: load-commands: the slice uses LC_DYLD_INFO, not LC_DYLD_INFO_ONLY, $not_read"
# load-commands runs before every other rule: a form not read beside a flat namespace, a flat
# lookup, an unbounded weak bind or a library outside the allowlist still names the form.
for c in 'a flat namespace:"minos": "11.0"' 'a bind through a flat lookup:"minos": "11.0", "bind_hex": "3e405f660090"' \
  'a weak bind of _getenv:"minos": "11.0", "weak_bind_hex": "405f676574656e760090"' \
  'another library:"minos": "11.0", "dylibs": [["LC_LOAD_DYLIB", "/opt/other/libother.dylib"]]' \
  'a raised minimum:"minos": "12.0"' 'an LC_RPATH:"minos": "11.0", "rpath": "/opt/other"'; do
  python3 -I "$here/make_macho.py" "$tmp/tiny.dylib" "{\"exports\": $exports, ${c#*:}, \"command\": $((0x80000034))}"
  if [[ "${c%%:*}" == "a flat namespace" ]]; then patch "$tmp/tiny.dylib" "$tmp/tiny.dylib" flags x86_64 0x100185; fi
  policy "$tmp/tiny.dylib" "$tp"
  expect "chained fixups beside ${c%%:*} fail at load-commands first" 1 "x86_64: load-commands: the slice uses chained fixups (LC_DYLD_CHAINED_FIXUPS)"
done
# An unrecognised command beside a flat lookup fails at load-commands too.
tiny '"minos": "11.0", "command": 153, "bind_hex": "3e405f660090"'
expect "an unrecognised load command beside a flat lookup fails at load-commands first" 1 \
  "x86_64: load-commands: the slice has a load command this tool does not recognise (0x99)"
# Exactly one LC_DYLD_INFO_ONLY, the command dyld reads binds from.
tiny '"minos": "11.0", "trie": "none"'
expect "a slice without LC_DYLD_INFO_ONLY fails at the namespace rule" 1 \
  "x86_64: namespace: the slice has 0 LC_DYLD_INFO_ONLY commands; dyld reads its binds from one"
tiny "\"dylibs\": [[\"LC_LOAD_DYLIB\", \"/usr/lib/libSystem.B.dylib\"], [\"LC_LOAD_DYLIB\", \"/opt/other/libother.dylib\"]]"
expect "an LC_LOAD_DYLIB naming a library outside the allowlist fails" 1 \
  "x86_64: linked-libraries: LC_LOAD_DYLIB loads /opt/other/libother.dylib, which is not in allowed-load-dylibs.txt"
# The other four library load forms are forms 2.3.0.0 does not use, refused by name even for an
# allowed library.
for form in LC_LOAD_WEAK_DYLIB LC_REEXPORT_DYLIB LC_LOAD_UPWARD_DYLIB LC_LAZY_LOAD_DYLIB; do
  tiny "\"dylibs\": [[\"LC_LOAD_DYLIB\", \"/usr/lib/libSystem.B.dylib\"], [\"$form\", \"/usr/lib/libc++.1.dylib\"]]"
  expect "$form, a load form 2.3.0.0 does not use, fails at load-commands" 1 \
    "x86_64: load-commands: the slice uses $form, which this tool does not read; a reviewed change adds it"
done
tiny '"minos": "11.0", "trie_size": 0'
expect "an LC_DYLD_INFO_ONLY with an empty export trie fails at exports" 1 "x86_64: exports: the slice has no export trie"
tiny '"trie_loop": true'
expect "an export trie that loops fails" 1 "x86_64: exports: the export trie is not one this tool reads: the export trie loops"
tiny '"trie_out": true'
expect "an export trie that points outside itself fails" 1 "x86_64: exports: the export trie is not one this tool reads: the export trie is deeper than 1024 or points outside itself"
for f in 2 8 16 1; do
  tiny "\"export_flags\": $f"
  expect "an export with flags $f, a kind 2.3.0.0 does not use, fails" 1 "x86_64: exports: the export trie is not one this tool reads: an export has flags this tool does not know (0x$(printf %x "$f"))"
done
tiny '"export_flags": 64'
expect "export flags this tool does not know fail" 1 "x86_64: exports: the export trie is not one this tool reads: an export has flags this tool does not know (0x40)"
tiny '"command": 153'
expect "an unrecognised load command fails" 1 "x86_64: load-commands: the slice has a load command this tool does not recognise (0x99)"
tiny '"rpath": "/opt/other"'
expect "an LC_RPATH fails" 1 "x86_64: search-path: the slice has an LC_RPATH"
tiny '"minos": "11.8"'
expect "a minimum macOS above min-os.txt's driver row fails" 1 "x86_64: min-os: the slice needs macOS 11.8.0, newer than 11.7.0 in min-os.txt"
tiny '"minos": "11.0"' "$(entry_points_json SQLExtra)"
expect "an extra export fails" 1 "x86_64: exports: the slice's exports differ from expected-exports.txt; extra: SQLExtra; missing: "
tiny '"minos": "11.0"' "$(entry_points_json -SQLFetch)"
expect "a missing export fails" 1 "x86_64: exports: the slice's exports differ from expected-exports.txt; extra: ; missing: SQLFetch"
grep -vx SQLFetch "$tp/expected-exports.txt" > "$tmp/without"; cp "$tmp/without" "$tp/expected-exports.txt"
tiny '"minos": "11.0"' "$(entry_points_json -SQLFetch)"
expect "an interface entry point missing from the list fails" 1 "entry-points: the interface's entry points are not all in expected-exports.txt for both slices; missing: SQLFetch"
echo "SQLFetch  x86_64" >> "$tp/expected-exports.txt"
tiny '"minos": "11.0"' "$(entry_points_json -SQLFetch)"
expect "an entry point listed for one slice only fails" 1 "entry-points: the interface's entry points are not all in expected-exports.txt for both slices; missing: SQLFetch"
entry_points > "$tp/expected-exports.txt"
printf 'duckdb  11.0\ndriver  10.15\n' > "$tp/min-os.txt"
tiny '"minos": "10.15"'
expect "a driver floor below DuckDB's fails as a policy error" 1 "policy: min-os.txt needs a duckdb and a driver row, the driver row not below duckdb's"
printf 'duckdb  11.0\ndriver  11.x\n' > "$tp/min-os.txt"
tiny '"minos": "11.0"'
expect "a min-os.txt row that is not a version fails" 1 "policy: min-os.txt:2: '11.x' is not a version such as 11.0"
cp "$odbc/policy/macos/min-os.txt" "$tp/"
echo "SQLFetch  x86_64  arm64" >> "$tp/expected-exports.txt"
tiny '"minos": "11.0"'
expect "an expected-exports.txt row with more than one slice fails" 1 "policy: expected-exports.txt:"
entry_points > "$tp/expected-exports.txt"
echo "SQLFetch  arm64e" >> "$tp/expected-exports.txt"
tiny '"minos": "11.0"'
expect "an expected-exports.txt row naming another slice fails in one line" 1 \
  "policy: expected-exports.txt:$(($(wc -l < "$tp/expected-exports.txt"))): a row is <name> [x86_64|arm64]" # BSD wc pads its count
entry_points > "$tp/expected-exports.txt"
# min-os.txt's rows and versions: as LC_BUILD_VERSION encodes them, at most 65535.255.255.
for v in 11 11.0.0.0 11.256 11.0.256 65536.0; do
  printf 'duckdb  11.0\ndriver  %s\n' "$v" > "$tp/min-os.txt"; tiny '"minos": "11.0"'
  expect "a min-os.txt version $v fails as a policy error" 1 "policy: min-os.txt:2: '$v' is not a version such as 11.0"
done
for rows in 'duckdb  11.0\ndriver  11.7  extra' 'duckdb  11.0\ndriver  11.7  provisional' 'duckdb  11.0\nduckdb  11.0\ndriver  11.7' \
  'driver  11.7' 'duckdb  11.0\nother  11.7'; do
  printf "$rows\n" > "$tp/min-os.txt"; tiny '"minos": "11.0"'
  expect "min-os.txt rows '${rows//\\n/; }' fail as a policy error" 1 "policy: min-os.txt"
done
printf 'duckdb  11.0\ndriver  11.7\n' > "$tp/min-os.txt"; tiny '"minos": "11.5"'
expect "a driver row above DuckDB's passes, and the summary says the minimum is newer than DuckDB's" 0 \
  "minimum macOS 11.5.0, newer than DuckDB's 11.0.0, as min-os.txt accepts)"
cp "$odbc/policy/macos/min-os.txt" "$tp/"
mv "$tp/allowed-load-dylibs.txt" "$tmp/allowed.saved"; tiny '"minos": "11.0"'
expect "a missing policy file fails as a policy error" 1 "policy: cannot read $tp/allowed-load-dylibs.txt"
mv "$tmp/allowed.saved" "$tp/allowed-load-dylibs.txt"
python3 -I "$here/make_macho.py" "$tmp/tiny.dylib" "{\"exports\": $exports, \"minos\": \"11.0\"}"
out_text="$(python3 -I "$lib/macho.py" policy "$tmp/tiny.dylib" "$tp" "$tmp/no-such.hpp" 2>&1)" && status=0 || status=$?
expect "an unreadable interface header fails at the entry points" 1 "entry-points: cannot read the interface header $tmp/no-such.hpp"
printf '#define OTHER_LIST(X) X(Fetch)\n' > "$tmp/other.hpp"
out_text="$(python3 -I "$lib/macho.py" policy "$tmp/tiny.dylib" "$tp" "$tmp/other.hpp" 2>&1)" && status=0 || status=$?
expect "a header without the entry-point list fails" 1 "entry-points: $tmp/other.hpp has no REDSHIFT_ODBC_ENTRY_POINTS(X) list"
# Two-level namespaces: MH_TWOLEVEL (0x80) set and MH_FORCE_FLAT (0x100) clear, in each slice.
for c in "x86_64 0x100005" "arm64 0x100005" "x86_64 0x100185" "arm64 0x100185"; do
  python3 -I "$here/make_macho.py" "$tmp/tiny.dylib" "{\"exports\": $exports, \"minos\": \"11.0\"}"
  read -r arch flags <<<"$c"
  patch "$tmp/tiny.dylib" "$tmp/p.dylib" flags "$arch" "$flags"; policy "$tmp/p.dylib" "$tp"
  expect "a $arch slice with header flags $flags fails at the namespace rule" 1 "$arch: namespace: the slice's flags $flags do not name two-level namespaces"
done
# Bind ordinals: each bind and lazy bind must name a library the slice loads
# (the tiny library loads one, libSystem). Opcodes: 0x1n ordinal n, 0x20 ordinal ULEB, 0x3n a
# special ordinal (0x30 itself, 0x3f the host, 0x3e flat, 0x3d weak), 0x40 a symbol name (0x48 one
# the slice defines non-weak), 0x90 bind, 0x00 done.
tiny '"minos": "11.0", "bind_hex": "11405f660090", "lazy_bind_hex": "11405f66009000"'
expect "binds and lazy binds of the library the slice loads pass" 0 "the library fits the macOS policy (minimum macOS 11.0.0)"
# One command: dyld reads one LC_DYLD_INFO_ONLY, so a bind by ordinal of _getenv in one cannot
# vouch for a weak bind of it in a second LC_DYLD_INFO_ONLY holding the weak-bind stream alone.
tiny '"minos": "11.0", "second_info": true, "bind_hex": "11405f676574656e760090", "weak_bind_hex": "405f676574656e760090"'
expect "a slice with two LC_DYLD_INFO_ONLY fails at the namespace rule" 1 \
  "x86_64: namespace: the slice has 2 LC_DYLD_INFO_ONLY commands; dyld reads its binds from one"
# A lazy-bind stream ends each entry with a done opcode; an entry after the first is read too.
tiny '"minos": "11.0", "lazy_bind_hex": "11405f660090003e405f67009000"'
expect "a lazy bind through a flat lookup after a good first entry fails" 1 \
  "x86_64: namespace: a lazy-bind opcode names a flat lookup (ordinal -2), not one of the slice's libraries (1 to 1)"
# Each LC_LOAD_DYLIB is one ordinal, as dyld counts them: ordinal 2 names the second.
libs="\"dylibs\": [[\"LC_LOAD_DYLIB\", \"/usr/lib/libSystem.B.dylib\"], [\"LC_LOAD_DYLIB\", \"/usr/lib/libc++.1.dylib\"]]"
tiny "\"minos\": \"11.0\", $libs, \"bind_hex\": \"12405f660090\""
expect "a bind of ordinal 2, the second LC_LOAD_DYLIB, passes" 0 "the library fits the macOS policy"
tiny "\"minos\": \"11.0\", $libs, \"bind_hex\": \"13405f660090\""
expect "a bind of ordinal 3 beside two LC_LOAD_DYLIB fails" 1 \
  "x86_64: namespace: a bind opcode names no library (ordinal 3), not one of the slice's libraries (1 to 2)"
# Weak binds: dyld resolves them by name, so the rule counts each name of the weak-bind opcodes
# once, and allows a name, exactly as the slice spells it, only when the slice exports it (its
# export trie, which the exports rule pins to expected-exports.txt) or also binds it by ordinal to
# one of its libraries. A -3 bind or lazy bind still fails (below). _SQLFetch is
# 5f53514c4665746368, _f 5f66, _getenv 5f676574656e76.
weak_why="weak-binds: the slice binds names by weak lookup that it neither exports nor binds by ordinal to one of its libraries, so dyld would find them by name alone"
counted="the library fits the macOS policy (minimum macOS 11.0.0); by weak lookup, which dyld resolves by name across the process, it binds 1 name in x86_64 and 1 in arm64"
tiny '"minos": "11.0", "weak_bind_hex": "405f53514c46657463680090405f53514c46657463680090"'
expect "a weak bind of an exported name passes, counted once though bound twice" 0 "$counted"
tiny '"minos": "11.0", "bind_hex": "11405f660090", "weak_bind_hex": "405f660090"'
expect "a weak bind of a name the slice also binds by ordinal passes" 0 "$counted"
tiny '"minos": "11.0", "lazy_bind_hex": "11405f66009000", "weak_bind_hex": "405f660090"'
expect "a weak bind of a name the slice also lazily binds by ordinal passes" 0 "$counted"
tiny '"minos": "11.0", "weak_bind_hex": "405f676574656e760090"'
expect "a weak bind of a name the slice neither exports nor binds by ordinal fails, naming it" 1 "x86_64: $weak_why: _getenv"
tiny '"minos": "11.0", "bind_hex": "11405f6600", "weak_bind_hex": "405f660090"'
expect "a name the bind opcodes set but never bind is not bound by ordinal" 1 "x86_64: $weak_why: _f"
tiny '"minos": "11.0", "lazy_bind_hex": "11405f66000011900000", "weak_bind_hex": "405f660090"'
expect "a name a lazy-bind entry sets is not the name of the next entry's bind" 1 "x86_64: $weak_why: _f"
# dyld stops the bind and weak-bind streams at their first done opcode, so a bind after it binds
# nothing and a weak-bind entry after it is no weak bind.
tiny '"minos": "11.0", "bind_hex": "0011405f676574656e760090", "weak_bind_hex": "405f676574656e760090"'
expect "a bind after the bind stream's done opcode is not a bind by ordinal" 1 "x86_64: $weak_why: _getenv"
tiny '"minos": "11.0", "bind_hex": "11405f6600900011405f676574656e760090", "weak_bind_hex": "405f676574656e760090"'
expect "a bind after the bind stream's first entry and done opcode is not a bind by ordinal" 1 "x86_64: $weak_why: _getenv"
tiny '"minos": "11.0", "bind_hex": "11405f660090003e405f670090", "weak_bind_hex": "405f53514c4665746368009000405f676574656e760090"'
expect "a flat lookup after the bind stream's done opcode, and a weak bind after the weak-bind stream's, are not read" 0 "$counted"
tiny '"minos": "11.0", "bind_hex": "11405f660090", "weak_bind_hex": "405f66009000485f53514c466574636800"'
expect "a non-weak definition flagged after the weak-bind stream's done opcode is not read" 0 "$counted"
# Names compare exactly as the slice spells them: SQLFetch (53514c4665746368) without its
# underscore is not the export _SQLFetch, nor _f the name f bound by ordinal.
tiny '"minos": "11.0", "weak_bind_hex": "4053514c46657463680090"'
expect "a weak bind of an export's name without its underscore fails" 1 "x86_64: $weak_why: SQLFetch"
tiny '"minos": "11.0", "bind_hex": "1140660090", "weak_bind_hex": "405f660090"'
expect "a weak bind of _f is not the name f bound by ordinal" 1 "x86_64: $weak_why: _f"
# A name with the weak-import flag (0x41, or 0x49 with the 0x8 bit) may name a library that lacks
# it, which dyld then binds to 0; 2.3.0.0 has none, so it fails in every stream, by name.
weak_import="has the weak-import flag, a form 2.3.0.0 does not use; a reviewed change adds it"
tiny '"minos": "11.0", "bind_hex": "11415f660090"'
expect "a bind with the weak-import flag fails at namespace" 1 "x86_64: namespace: the bind opcodes are not ones this tool reads: _f $weak_import"
tiny '"minos": "11.0", "bind_hex": "11405f670090495f660090"'
expect "a later bind flagged by 0x49 fails at namespace" 1 "x86_64: namespace: the bind opcodes are not ones this tool reads: _f $weak_import"
tiny '"minos": "11.0", "lazy_bind_hex": "11415f66009000"'
expect "a lazy bind with the weak-import flag fails at namespace" 1 "x86_64: namespace: the lazy-bind opcodes are not ones this tool reads: _f $weak_import"
tiny '"minos": "11.0", "weak_bind_hex": "415f53514c46657463680090"'
expect "a weak-bind entry with the weak-import flag fails at weak-binds" 1 \
  "x86_64: weak-binds: the weak-bind opcodes are not ones this tool reads: _SQLFetch $weak_import"
# The exports rule pins the export trie the weak-binds rule reads: an export the list lacks fails
# there, weak-bound or not. NewTemplate is 5f4e657754656d706c617465.
tiny '"minos": "11.0", "weak_bind_hex": "405f4e657754656d706c6174650090"' "$(entry_points_json NewTemplate)"
expect "a weak-bound export that expected-exports.txt lacks fails at the exports rule" 1 \
  "x86_64: exports: the slice's exports differ from expected-exports.txt; extra: NewTemplate; missing: "
# libc++'s names get no exception: operator new (__Znwm, 5f5f5a6e776d), which libc++ defines and
# 2.3.0.0 binds by ordinal too, passes only so, in a slice that loads libc++ or not.
libcxx="\"dylibs\": [[\"LC_LOAD_DYLIB\", \"/usr/lib/libSystem.B.dylib\"], [\"LC_LOAD_DYLIB\", \"/usr/lib/libc++.1.dylib\"]]"
tiny "\"minos\": \"11.0\", $libcxx, \"weak_bind_hex\": \"405f5f5a6e776d0090\""
expect "a weak bind alone of operator new fails, though the slice loads libc++" 1 "x86_64: $weak_why: __Znwm"
tiny "\"minos\": \"11.0\", $libcxx, \"bind_hex\": \"12405f5f5a6e776d0090\", \"weak_bind_hex\": \"405f5f5a6e776d0090\""
expect "a weak bind of operator new it also binds by ordinal from libc++ passes, as in 2.3.0.0" 0 "$counted"
# Each bind opcode binds: 0x90, 0xa0 (then a ULEB), 0xb0 and 0xc0 (then two ULEBs).
for op in 90 a000 b0 c00101; do
  tiny "\"minos\": \"11.0\", \"bind_hex\": \"11405f6600${op}\", \"weak_bind_hex\": \"405f660090\""
  expect "a name bound by ordinal with bind opcode $op may be weak-bound" 0 "$counted"
  tiny "\"minos\": \"11.0\", \"bind_hex\": \"405f6600${op}\""
  expect "bind opcode $op before any library ordinal fails" 1 \
    "x86_64: namespace: a bind opcode binds before its entry names a library, so dyld would bind it to the library itself (ordinal 0)"
done
many="$(python3 -I -c 'print("".join("405f%02x0090" % c for c in range(0x61, 0x76)))')"
tiny "\"minos\": \"11.0\", \"weak_bind_hex\": \"$many\""
expect "21 such names fail, the first 20 named" 1 "x86_64: $weak_why: _a _b _c _d _e _f _g _h _i _j _k _l _m _n _o _p _q _r _s _t ..."
# Each slice is judged by its own rows: SQLOnlyX (5f53514c4f6e6c7958) is an x86_64 export alone.
{ entry_points; echo "SQLOnlyX  x86_64"; } > "$tp/expected-exports.txt"
x86="$(entry_points_json SQLOnlyX)"
tiny '"minos": "11.0", "weak_bind_hex": "405f53514c4f6e6c79580090"' "$x86, \"arm64_exports\": $exports"
expect "a weak bind of a name only the other slice exports fails" 1 "arm64: $weak_why: _SQLOnlyX"
entry_points > "$tp/expected-exports.txt"
# The non-weak-definition flag is a bit of the name opcode's immediate, as dyld reads it: 0x4c sets
# it beside 0x4.
for imm in 48 4c; do
  tiny "\"minos\": \"11.0\", \"weak_bind_hex\": \"${imm}5f53514c466574636800\""
  expect "a weak-bind entry flagged by 0x$imm as the slice's own non-weak definition fails" 1 \
    "x86_64: weak-binds: the weak-bind opcodes flag names as the slice's own non-weak definitions, which would take the place of every other image's weak definition of them: _SQLFetch"
done
# A bind needs a library ordinal set in its entry (dyld starts each at ordinal 0, the library
# itself, and each lazy-bind entry after a done opcode).
tiny '"minos": "11.0", "bind_hex": "405f660090"'
expect "a bind before any library ordinal fails" 1 \
  "x86_64: namespace: a bind opcode binds before its entry names a library, so dyld would bind it to the library itself (ordinal 0)"
tiny '"minos": "11.0", "lazy_bind_hex": "11405f66009000405f67009000"'
expect "a lazy bind whose entry sets no library ordinal after a good first entry fails" 1 \
  "x86_64: namespace: a lazy-bind opcode binds before its entry names a library, so dyld would bind it to the library itself (ordinal 0)"
for c in "d0:opcode 0xd0 is not one this tool reads" "405f66:a symbol name runs past the stream" "7080:a number in the weak-bind opcode stream runs past it"; do
  tiny "\"minos\": \"11.0\", \"weak_bind_hex\": \"${c%%:*}\""
  expect "a weak-bind stream ${c%%:*} fails as one this tool does not read" 1 \
    "x86_64: weak-binds: the weak-bind opcodes are not ones this tool reads: ${c#*:}"
done
tiny '"minos": "11.0", "weak_bind_hex": "405f660090", "weak_bind_size": 1000000'
expect "a weak-bind stream past the slice fails" 1 "x86_64: weak-binds: the weak-bind opcodes lie outside the slice"
for c in "bind 30:the library itself (ordinal 0)" "bind 3f:the host executable (ordinal -1)" "bind 3e:a flat lookup (ordinal -2)" \
  "bind 3d:a weak lookup (ordinal -3)" "bind 12:no library (ordinal 2)" "lazy-bind 3d:a weak lookup (ordinal -3)" \
  "lazy-bind 30:the library itself (ordinal 0)" "lazy-bind 2009:no library (ordinal 9)" \
  "lazy-bind 2080808001:no library (ordinal 2097152)"; do
  read -r stream rest <<<"$c"; ord="${rest%%:*}" why="${rest#*:}"
  key=bind_hex; [[ "$stream" == bind ]] || key=lazy_bind_hex
  tiny "\"minos\": \"11.0\", \"$key\": \"${ord}405f660090\""
  expect "a $stream opcode naming $why fails at the namespace rule" 1 \
    "x86_64: namespace: a $stream opcode names $why, not one of the slice's libraries (1 to 1)"
done
for c in "d0:opcode 0xd0 is not one this tool reads" "405f66:a symbol name runs past the stream" \
  "2080:a number in the bind opcode stream runs past it" "6080:a number in the bind opcode stream runs past it" \
  "c00180:a number in the bind opcode stream runs past it"; do
  tiny "\"minos\": \"11.0\", \"bind_hex\": \"${c%%:*}\""
  expect "a bind stream ${c%%:*} fails as one this tool does not read" 1 \
    "x86_64: namespace: the bind opcodes are not ones this tool reads: ${c#*:}"
done
tiny '"minos": "11.0", "bind_hex": "11", "bind_size": 1000000'
expect "a bind stream past the slice fails" 1 "x86_64: namespace: the bind opcodes lie outside the slice"
# Export tries the reader must find, and ones it must refuse.
tiny '"minos": "11.0", "trie_size": 1000000'
expect "an export trie past the slice fails" 1 "x86_64: exports: the export trie lies outside the slice"
for t in "80:a number in the export trie runs past it" "00:a node of the export trie runs past it" \
  "0001616263:an edge of the export trie has no end"; do
  tiny "\"minos\": \"11.0\", \"trie_hex\": \"${t%%:*}\""
  expect "an export trie of bytes ${t%%:*} fails" 1 "x86_64: exports: the export trie is not one this tool reads: ${t#*:}"
done
deep="$(python3 -I -c 'out = b""
for i in range(1100):
    out += b"\x00\x01a\x00" + bytes([(i + 1) * 6 & 0x7F | 0x80, (i + 1) * 6 >> 7])
print((out + b"\x00\x00").hex())')"
tiny "\"minos\": \"11.0\", \"trie_hex\": \"$deep\""
expect "an export trie deeper than 1024 fails" 1 "x86_64: exports: the export trie is not one this tool reads: the export trie is deeper than 1024"
# The minimum macOS: two LC_BUILD_VERSION commands fail (LC_VERSION_MIN_MACOSX is a form not read).
tiny '"minos": "11.0", "version_command": "two"'
expect "two LC_BUILD_VERSION commands fail" 1 "x86_64: min-os: the slice does not name exactly one minimum macOS version"
python3 -I "$here/make_macho.py" "$tmp/tiny.dylib" "{\"exports\": $exports, \"minos\": \"11.0\"}"
patch "$tmp/tiny.dylib" "$tmp/p.dylib" dylib-fill x86_64; policy "$tmp/p.dylib" "$tp"
expect "a library name without its NUL fails" 1 "x86_64: linked-libraries: a load command's library name lies outside it"
# The arm64 slice on its own: a tiny library with one change in arm64 only fails, naming arm64.
for change in "dylib arm64 /opt/other/libother.dylib:arm64: linked-libraries: LC_LOAD_DYLIB loads /opt/other/libother.dylib" \
  "rpath arm64 /opt/other:arm64: search-path: the slice has an LC_RPATH" \
  "minos arm64 11.8.0:arm64: min-os: the slice needs macOS 11.8.0" "platform arm64 2:arm64: min-os: the slice does not name exactly one" \
  "export arm64 _SQLFetch:arm64: exports: the slice's exports differ from expected-exports.txt; extra: SQLFetcY; missing: SQLFetch" \
  "dylib-offset arm64 8:arm64: linked-libraries: a load command's library name lies outside it" \
  "dylib-offset arm64 4096:arm64: linked-libraries: a load command's library name lies outside it"; do
  python3 -I "$here/make_macho.py" "$tmp/tiny.dylib" "{\"exports\": $exports, \"minos\": \"11.0\"}"
  read -r -a args <<<"${change%%:*}"
  patch "$tmp/tiny.dylib" "$tmp/p.dylib" "${args[@]}"; policy "$tmp/p.dylib" "$tp"
  expect "a tiny library whose arm64 slice alone has '${change%%:*}' fails at arm64" 1 "${change#*:}"
done

# The environment-name rule, with the repository's prefixes, one refused and one known name, and
# no ignored strings.
te="$tmp/env"; mkdir -p "$te"
cp "$odbc/policy/environment-prefixes.txt" "$te/"
printf 'SSLKEYLOGFILE  load  # only in linux_amd64,linux_arm64\n' > "$te/refused-environment.txt"
: > "$te/ignored-strings.txt"
printf 'SSL_CERT_FILE  CaFile\nOPENSSL_CONF  -  a listed name outside the scanned prefixes\n' > "$te/known-environment.txt"
strings_lib() { python3 -I "$here/make_macho.py" "$tmp/s.dylib" "{\"strings\": $1}"; envrule "$tmp/s.dylib" "$te"; }
# expect_slices <name> <substring>: the rule fails with <substring> for each slice, x86_64 and then
# arm64, whose line is the last.
expect_slices() {
  if [[ "$out_text" == *"x86_64: $2"* ]]; then expect "$1" 1 "arm64: $2"; else fail "$1 (no x86_64 line; exit $status; output: $out_text)"; fi
}
# expect_listed <name> <substring>: the rule fails and lists <substring> among its problems; the
# environment rule prints every problem it finds, so a given one need not be the last.
expect_listed() {
  if [[ "$status" == 1 && "$out_text" == *"$2"* ]]; then pass "$1"; else fail "$1 (exit $status; output: $out_text)"; fi
}
strings_lib '["SSL_CERT_FILE", "PGRES_TUPLES_OK", "SSLKEYLOGFILE"]'
expect "known and refused names, and a PGRES_ name, pass" 0 "x86_64: the driver's 2 scanned names are all classified"
strings_lib '["SSL_CERT_FILE", "SSL_CTX_new", "ssl_LOWER"]'
expect "a name that leaves its prefix's case is not a variable name and passes" 0 "x86_64: the driver's 1 scanned names are all classified"
strings_lib '["SSL_CERT_FILE", "PGNEWNAME"]'
expect_slices "a new PG name fails, naming it" "environment: the driver names PGNEWNAME, which no policy list classifies"
strings_lib '["SSL_CERT_FILE", "SSLNEWNAME"]'
expect_slices "a new name under a prefix fails, naming it" "environment: the driver names SSLNEWNAME, which no policy list classifies"
strings_lib '["SSL_CERT_FILE", "idp_use_https_proxy"]'
expect_slices "a name stored as the tail of a longer string is found" "environment: the driver names https_proxy, which no policy list classifies"
strings_lib '["PGRES_TUPLES_OK"]'
expect_slices "a known name the library no longer holds fails" "environment: SSL_CERT_FILE (known-environment.txt:1) is no longer in the driver; remove its row"
printf 'AWS_SOME_ERROR  an error name\n' > "$te/ignored-strings.txt"
strings_lib '["SSL_CERT_FILE", "AWS_SOME_ERROR"]'
expect "an ignored string the library holds passes" 0 "x86_64: the driver's 2 scanned names are all classified"
strings_lib '["SSL_CERT_FILE"]'
expect_slices "an ignored string the library no longer holds fails" "environment: AWS_SOME_ERROR (ignored-strings.txt:1) is no longer in the driver; remove its row"
printf 'AWS_SOME_ERROR  an error name  # only in linux_amd64,linux_arm64\n' > "$te/ignored-strings.txt"
strings_lib '["SSL_CERT_FILE"]'
expect "a row only for Linux need not be in the macOS library" 0 "x86_64: the driver's 1 scanned names are all classified"
printf 'AWS_SOME_ERROR  an error name  # only in osx_amd64; x86_64 only\n' > "$te/ignored-strings.txt"
strings_lib '["SSL_CERT_FILE"]'
expect "a row only for osx_amd64 must be in the x86_64 slice" 1 "x86_64: environment: AWS_SOME_ERROR (ignored-strings.txt:1) is no longer in the driver; remove its row"
[[ "$out_text" != *"arm64: environment: AWS_SOME_ERROR"* ]] && pass "a row only for osx_amd64 is not required of the arm64 slice" ||
  fail "a row only for osx_amd64 is not required of the arm64 slice ($out_text)"
strings_lib '["SSL_CERT_FILE", "AWS_SOME_ERROR"]'
expect "a row only for osx_amd64 does not classify the string in the arm64 slice" 1 \
  "arm64: environment: the driver names AWS_SOME_ERROR, whose row (ignored-strings.txt:1) is only in osx_amd64"
[[ "$out_text" != *"x86_64: environment"* ]] && pass "the same row classifies it in the x86_64 slice" ||
  fail "the same row classifies it in the x86_64 slice ($out_text)"
printf 'AWS_SOME_ERROR  an error name  # only in osx_x86_64\n' > "$te/ignored-strings.txt"
strings_lib '["SSL_CERT_FILE", "AWS_SOME_ERROR"]'
expect_listed "a slice's architecture is not a platform name" "policy: ignored-strings.txt:1: \"only in\" names platforms from linux_amd64, linux_arm64, osx_amd64, osx_arm64"
printf 'SSL_CERT_FILE\n' > "$te/known-environment.txt"
strings_lib '["SSL_CERT_FILE"]'
expect_listed "a known row without a keyword or a reason fails" "policy: known-environment.txt:1: a known row is <NAME> <Keyword>"

# Through verify_macos.sh, in a tree pinning the test PKI: a signed package whose library breaks
# the loading policy, or the environment-name rule, fails at that step.
if sha1_refused; then skip "the signed-package cases" "$no_sha1"; finish; fi
mpki="$tmp/mpki"; make_pki "$mpki" || { fail "a test PKI is generated"; finish; }
tree="$tmp/tree"; mac_tree "$tree" "$mpki"; serve_notices
signed_package() { # signed_package <make_macho spec keys>
  python3 -I "$here/make_macho.py" "$tmp/lib.dylib" "{$1}"
  python3 -I "$here/make_xar.py" "$FAKE_CURL_DIR/AmazonRedshiftODBC-64-bit.$version.universal.pkg" \
    "{\"sign\": {\"key\": \"$mpki/installer.key\", \"certs\": [\"$mpki/installer.pem\", \"$mpki/int.pem\", \"$mpki/root.pem\"]}, \"members\": [[\"./librsodbc64.dylib\", \"file\", 1, \"@$tmp/lib.dylib\"]]}"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" bash "$tree/tools/lib/verify_macos.sh" osx_arm64 "$version" "$tree" "$header" 2>&1)" && status=0 || status=$?
}
signed_package "\"exports\": $exports, \"strings\": [\"SSL_CERT_FILE\"], \"dylibs\": [[\"LC_LOAD_DYLIB\", \"/opt/other/libother.dylib\"]]"
expect "verification fails at the loading policy for a signed library that loads another library" 1 "verify-release: osx_arm64: x86_64: linked-libraries: LC_LOAD_DYLIB loads /opt/other/libother.dylib"
signed_package "\"exports\": $exports, \"strings\": [\"SSL_CERT_FILE\", \"PGNEWNAME\"]"
expect "verification fails at the environment-name rule for a signed library with a new name, at its first slice" 1 \
  "verify-release: osx_arm64: x86_64: environment: the driver names PGNEWNAME"
signed_package "\"exports\": $exports, \"strings\": [\"SSL_CERT_FILE\", \"PGNEWNAME\", \"PGOTHERNAME\"]"
[[ "$status" == 1 && "$(grep -c '^verify-release: osx_arm64: x86_64: environment: the driver names PG' <<<"$out_text")" == 2 ]] &&
  pass "every environment problem of a slice is printed, each on its own line led by the platform and slice" ||
  fail "two environment problems (exit $status; $out_text)"
signed_package "\"exports\": $exports, \"strings\": [\"SSL_CERT_FILE\"]"
expect "the same library without them passes and prints the rows" 0 "osx_arm64  driver  librsodbc64.dylib  $(sha "$tmp/lib.dylib")"
[[ "$out_text" == *"osx_amd64  driver  librsodbc64.dylib  $(sha "$tmp/lib.dylib")"* ]] &&
  pass "the run prints the osx_amd64 row with the same hashes" || fail "the osx_amd64 row ($out_text)"
signed_package "\"exports\": $exports, \"strings\": [\"SSL_CERT_FILE\"], \"command\": $((0x80000034))"
expect "verification fails at load-commands for a signed library with chained fixups" 1 \
  "verify-release: osx_arm64: x86_64: load-commands: the slice uses chained fixups (LC_DYLD_CHAINED_FIXUPS)"
rm -f "$FAKE_CURL_LOG"
out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" bash "$tree/tools/lib/verify_macos.sh" osx_arm64 "$version" "$tree" "$tmp/no-header.hpp" 2>&1)" && status=0 || status=$?
[[ ! -e "$FAKE_CURL_LOG" ]] || status="$status, with a download"
expect "a missing interface header fails before any download" 1 "entry-points: the interface header $tmp/no-header.hpp is missing or unreadable"
# An interface header that exists but cannot be read; root reads it anyway, so then the case skips.
cp "$header" "$tmp/unreadable.hpp"; chmod 000 "$tmp/unreadable.hpp"
if [[ -r "$tmp/unreadable.hpp" ]]; then
  skip "an unreadable interface header fails before any download" "this user reads a mode-000 file (root)"
else
  rm -f "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" bash "$tree/tools/lib/verify_macos.sh" osx_arm64 "$version" "$tree" "$tmp/unreadable.hpp" 2>&1)" && status=0 || status=$?
  [[ ! -e "$FAKE_CURL_LOG" ]] || status="$status, with a download"
  expect "an unreadable interface header fails before any download" 1 "entry-points: the interface header $tmp/unreadable.hpp is missing or unreadable"
fi
chmod 600 "$tmp/unreadable.hpp"

finish
