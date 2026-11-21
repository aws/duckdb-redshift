#!/usr/bin/env bash
# Tool tests for the Windows loading policy: lib/windows_policy.py on each file
# of a set, and lib/env_names.py on the set as windows_amd64, as verify_windows.sh runs them. Every
# DLL is a small PE32+ image make_pe.py writes at test time, with its own policy files. A set that
# meets every rule passes, and rows reproduces its policy files; each of the policy
# cases, and an added environment name, fails at its rule. The locked 2.3.0.0 DLLs pass with the
# checked-in policy, from the MSI in the release cache or RS_DRIVER_MIRROR, never downloaded; the
# case skips while the lock under test has no windows_amd64 row or the MSI is not cached. Run by
# run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
odbc="$here/../../../third_party/redshift_odbc"
lib="$odbc/tools/lib"
header="$here/../../../src/transport/odbc_driver.hpp"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=release-cache.sh
source "$here/release-cache.sh" # release_cache_dir, sha
# shellcheck source=bounded.sh
source "$here/bounded.sh"

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
skip() { n=$((n+1)); echo "ok $n - $1 # SKIP $2"; } # counted, so every run has the same cases
# expect <name> <exit> <output>: the last run's whole output, stdout and stderr, must be <output>.
expect() {
  if [[ "$status" == "$2" && "$out_text" == "$3" ]]; then pass "$1"; else
    fail "$1 (exit $status, wanted $2; output: $out_text)"; fi
}
run() { out_text="$(bounded python3 -I "$@" 2>&1)" && status=0 || status=$?; }
# passes <name>: the last check passed the whole set.
passes() {
  local first="${out_text%%$'\n'*}"
  if [[ "$status" == 0 && "$first" == "the set's "*" files meet the Windows loading policy" ]]
  then pass "$1"; else fail "$1 (exit $status: $out_text)"; fi
}
# The problem a file's unlisted run-time name gives.
unlisted() {
  echo "runtime-names: $1: it holds the run-time name $2, which runtime-names.txt does not list"
}

# The entry points the interface header lists, which the driver must export.
entry_points="$(python3 -I -c 'import re, sys
text = open(sys.argv[1]).read()
block = re.search(r"#define REDSHIFT_ODBC_ENTRY_POINTS\(X\)((?:[^\n]*\\\n)*[^\n]*)", text).group(1)
print(" ".join("SQL" + n for n in re.findall(r"X\(([A-Za-z0-9_]+)\)", block)))' "$header")"
exported="$(printf '"%s", ' $entry_points)"
# Where the list puts SQLFetch, after its comment row, and how many entry points it holds: the
# messages below name both, so they follow the interface's table.
fetch_row=$(( $(tr ' ' '\n' <<<"$entry_points" | grep -nx SQLFetch | cut -d: -f1) + 1 ))
entry_count=$(wc -w <<<"$entry_points" | tr -d ' ')
exported="[${exported%, }]"

# The set: rsodbc.dll (the driver), crypto.dll and msvcr100.dll. Each spec is make_pe.py's. The
# driver's PDB path, crypto.dll's version resource and certificate table, and every import and
# delay-import name and hint/name entry hold names the run-time search must not report.
rsodbc_spec() { # rsodbc_spec <exported>: the driver's rdata, exporting <exported>
  echo "\"imports\": [\"KERNEL32.dll\", \"crypto.dll\", \"VCRUNTIME140.dll\"],
  \"imported_functions\": [\"SSL_CTX_new\", \"dbghelp\"], \"imported_ordinals\": 1,
  \"delay_imports\": [\"Secur32.dll\"], \"delay_functions\": [\"crypto\"],
  \"export_name\": \"rsodbc.dll\", \"exported\": $1, \"load_flags\": 2304,
  \"pdb\": \"C:\\\\build\\\\crypto.dll\",
  \"strings\": [[\"a\", \"sslmode\"], [\"u\", \"dbghelp.dll\"]]"
}
rsodbc_rdata="$(rsodbc_spec "$exported")"
crypto_rdata='"imports": ["KERNEL32.dll", "msvcr100.dll"], "export_name": "crypto.dll",
  "functions": 2'
crypto_more='"version": {"OriginalFilename": "crypto32.dll", "FileVersion": "2.3.0.0"},
  "certificate": "\u0000signed by msvcr100.dll\u0000"'
msvcr_rdata='"imports": ["KERNEL32.dll"], "export_name": "MSVCR100.dll", "functions": 1'
# dll <dir> <name> <rdata> [<more spec>]: writes <dir>/<name>.
dll() { mkdir -p "$1"; python3 -I "$here/make_pe.py" "$1/$2" "{\"rdata\": {$3}${4:+, $4}}"; }
good_set() { # good_set <dir>
  dll "$1" rsodbc.dll "$rsodbc_rdata"
  dll "$1" crypto.dll "$crypto_rdata" "$crypto_more"
  dll "$1" msvcr100.dll "$msvcr_rdata"
}
good_set "$tmp/set"

# The policy: the repository's unshipped names and prerequisites, with this set's Windows files.
policy="$tmp/policy"
mkdir -p "$policy/windows"
cp "$odbc/policy/windows/unshipped-names.txt" "$policy/windows/"
{ cat "$odbc/policy/host-prerequisites.txt"; echo "windows  VCRUNTIME140.dll"; } \
  > "$policy/host-prerequisites.txt"
good_imports=("crypto.dll  KERNEL32.dll" "crypto.dll  msvcr100.dll" "msvcr100.dll  KERNEL32.dll"
  "rsodbc.dll  crypto.dll" "rsodbc.dll  KERNEL32.dll" "rsodbc.dll  Secur32.dll" \
    "rsodbc.dll  VCRUNTIME140.dll")
imports_rows=("${good_imports[@]}")
runtime_rows=("rsodbc.dll  dbghelp.dll")
write_policy() {
  local w="$policy/windows"
  printf '%s\n' "# <file> <dll>" "${imports_rows[@]}" > "$w/allowed-imports.txt"
  printf '%s\n' "# one exported name per row" $entry_points > "$w/expected-exports.txt"
  printf '%s\n' "# <file> <string>" ${runtime_rows[@]+"${runtime_rows[@]}"} > "$w/runtime-names.txt"
}
write_policy
# check [<set-dir>]: the policy on a set, the good one unless another is given.
check() { run "$lib/windows_policy.py" check "${1:-$tmp/set}" rsodbc.dll "$policy" "$header"; }
# variant <file> <rdata> [<more spec>]: a copy of the good set with <file> written from its spec
# instead, checked.
variant() {
  rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"; rm -f -- "$tmp/v/$1"
  dll "$tmp/v" "$1" "$2" "${3:-}"; check "$tmp/v"
}

check
expect "a set that meets every rule passes, and the summary records each OriginalFilename" 0 \
  "the set's 3 files meet the Windows loading policy
crypto.dll: 2 imports, 0 delay imports, 0 forwarded DLLs, 0 run-time names, OriginalFilename crypto32.dll
msvcr100.dll: 1 imports, 0 delay imports, 0 forwarded DLLs, 0 run-time names, OriginalFilename none
rsodbc.dll: 3 imports, 1 delay imports, 0 forwarded DLLs, 1 run-time names, OriginalFilename none"
run "$lib/windows_policy.py" policy "$policy" "$header"
expect "its policy files are well formed and hold every entry point" 0 \
  "the Windows policy files are well formed, and expected-exports.txt holds every entry point"
for kind in allowed-imports expected-exports runtime-names; do
  run "$lib/windows_policy.py" rows "$tmp/set" rsodbc.dll "$policy" "$kind"
  want="$(grep -v '^#' "$policy/windows/$kind.txt" | LC_ALL=C sort -f)"
  if [[ "$status" == 0 && "$out_text" == "$want" ]]; then pass "rows reproduces $kind.txt"
  else fail "rows reproduces $kind.txt (exit $status: $out_text)"; fi
done

# The policy cases, each on the good set with one file changed.
with_import() { echo "${crypto_rdata/\"msvcr100.dll\"/\"msvcr100.dll\", \"$1\"}"; }
variant crypto.dll "$(with_import USER32.dll)" "$crypto_more"
expect "an added import fails" 1 \
  "imports: crypto.dll: it imports USER32.dll, which allowed-imports.txt does not list"
imports_rows+=("crypto.dll  ADVAPI32.dll"); write_policy; check
expect "an allowed import the file no longer has fails" 1 \
  "imports: crypto.dll: allowed-imports.txt:9 lists advapi32.dll, which it does not import"
imports_rows=("${good_imports[@]}"); write_policy
variant rsodbc.dll "${rsodbc_rdata/\"Secur32.dll\"/\"Secur32.dll\", \"SECUR32.dll\"}"
if [[ "$status" == 0 && "$out_text" == *"rsodbc.dll: 3 imports, 1 delay imports,"* ]]; then
  pass "the summary counts a DLL delay-imported twice, in two cases, once"
else fail "the summary counts a DLL delay-imported twice, in two cases, once (exit $status: $out_text)"; fi
# The delay helper loads a delay import by name, so allowed-imports.txt lists it too.
variant crypto.dll "$crypto_rdata, \"delay_imports\": [\"dbghelp.dll\"]" "$crypto_more"
expect "a delay import of a system DLL that allowed-imports.txt does not list fails" 1 \
  "imports: crypto.dll: it imports dbghelp.dll, which allowed-imports.txt does not list"
imports_rows+=("crypto.dll  RSODBC"); write_policy
variant crypto.dll "$crypto_rdata, \"delay_imports\": [\"RSODBC\"]" "$crypto_more"
expect "a delay import naming a set member fails" 1 \
  "delay-imports: crypto.dll: it delay-loads the set member RSODBC"
imports_rows=("${good_imports[@]}" "crypto.dll  rsodbc.dll"); write_policy
variant crypto.dll "$crypto_rdata, \"delay_imports\": [\"rsodbc.dll\"]" "$crypto_more"
expect "a delay import naming a set member by its file name, as a linker writes it, fails" 1 \
  "delay-imports: crypto.dll: it delay-loads the set member rsodbc.dll"
imports_rows=("${good_imports[@]}"); write_policy
# A "#" that starts a token starts a comment, as driver_info.py reads the policy files.
printf '%s\n' "rsodbc.dll  KERNEL32.dll  # kernel32" >> "$policy/windows/allowed-imports.txt"
cp "$policy/host-prerequisites.txt" "$tmp/prerequisites"
printf '%s\n' "windows  VCRUNTIME140.dll  # the VC++ runtime" >> "$policy/host-prerequisites.txt"
check
expect "a row with a comment after it is the row before the comment" 1 \
  "policy: allowed-imports.txt:9: KERNEL32.dll is listed twice for rsodbc.dll"
write_policy
check; passes "a windows prerequisite row with a comment after it is read as windows <dll>"
printf '%s\n' "windows  VCRUNTIME140.dll  extra" >> "$policy/host-prerequisites.txt"
check
expect "a windows prerequisite row of another form fails" 1 \
  "policy: host-prerequisites.txt:$(wc -l < "$policy/host-prerequisites.txt" | tr -d ' '): \
a windows row is windows <dll>"
cp "$tmp/prerequisites" "$policy/host-prerequisites.txt"
variant crypto.dll "$crypto_rdata, \"forwarders\": [\"MSVCR100.malloc\"]" "$crypto_more"
expect "a forwarder naming a set member fails" 1 \
  "forwarders: crypto.dll: it forwards MSVCR100.malloc to a set member"
variant crypto.dll "$crypto_rdata, \"forwarders\": [\"MSVCR100.dll.malloc\"]" "$crypto_more"
expect "a forwarder naming a set member by its file name fails" 1 \
  "forwarders: crypto.dll: it forwards MSVCR100.dll.malloc to a set member"
# The loader loads a forwarder's DLL by name, adding .dll, so allowed-imports.txt lists it too.
variant crypto.dll "$crypto_rdata, \"forwarders\": [\"evilhelper.SSL_new\"]" "$crypto_more"
expect "a forwarder to a DLL outside the set that allowed-imports.txt does not list fails" 1 \
  "imports: crypto.dll: it imports evilhelper.dll, which allowed-imports.txt does not list"
imports_rows+=("crypto.dll  NTDLL.dll"); write_policy
variant crypto.dll "$crypto_rdata, \"forwarders\": [\"NTDLL.RtlAllocateHeap\"]" "$crypto_more"
passes "a forwarder to a system DLL that allowed-imports.txt lists passes"
run "$lib/windows_policy.py" rows "$tmp/v" rsodbc.dll "$policy" allowed-imports
if [[ "$status" == 0 ]] && grep -qx 'crypto.dll  NTDLL.dll' <<<"$out_text"; then
  pass "rows allowed-imports prints a forwarder's DLL"
else fail "rows allowed-imports prints a forwarder's DLL (exit $status: $out_text)"; fi
# A target that already ends in .dll is not given a second one; a dotted one keeps its dots.
imports_rows+=("crypto.dll  my.helper.dll"); write_policy
variant crypto.dll "$crypto_rdata, \"forwarders\": [\"NTDLL.dll.RtlFreeHeap\", \"my.helper.f\"]" "$crypto_more"
passes "forwarders to NTDLL.dll.<name> and to a dotted DLL name, both listed, pass"
if [[ "$out_text" == *"crypto.dll: 2 imports, 0 delay imports, 2 forwarded DLLs, 0 run-time names"* ]]; then
  pass "the summary counts each forwarded DLL"
else fail "the summary counts each forwarded DLL ($out_text)"; fi
variant crypto.dll "$crypto_rdata, \"forwarders\": [\"NTDLL.RtlAllocateHeap\", \"ntdll.dll.RtlFreeHeap\",
  \"my.helper.f\"]" "$crypto_more"
if [[ "$out_text" == *"crypto.dll: 2 imports, 0 delay imports, 2 forwarded DLLs, 0 run-time names"* ]]; then
  pass "the summary counts forwarders to NTDLL and to ntdll.dll as the one DLL allowed-imports.txt lists"
else fail "the summary counts forwarders to NTDLL and to ntdll.dll as one DLL ($out_text)"; fi
imports_rows=("${good_imports[@]}"); write_policy
variant crypto.dll "$crypto_rdata, \"bound_import\": true" "$crypto_more"
expect "a bound import fails" 1 "bound-imports: crypto.dll: it has bound imports"
imports_rows+=("crypto.dll  MSVCP140.dll"); write_policy
cp "$policy/host-prerequisites.txt" "$tmp/prerequisites"
# Another operating system's row does not count.
echo "linux  MSVCP140.dll" >> "$policy/host-prerequisites.txt"
variant crypto.dll "$(with_import MSVCP140.dll)" "$crypto_more"
expect "an allowed import of a VC++ run-time DLL host-prerequisites.txt does not list fails" 1 \
  "prerequisites: crypto.dll: it imports the VC++ run-time DLL MSVCP140.dll, which \
host-prerequisites.txt does not list for windows"
imports_rows=("${good_imports[@]}"); write_policy
grep -iv '^windows *vcruntime140\.dll' "$tmp/prerequisites" > "$policy/host-prerequisites.txt"
check
expect "an import of VCRUNTIME140.dll that host-prerequisites.txt does not list fails" 1 \
  "prerequisites: rsodbc.dll: it imports the VC++ run-time DLL VCRUNTIME140.dll, which \
host-prerequisites.txt does not list for windows"
cp "$tmp/prerequisites" "$policy/host-prerequisites.txt"
variant rsodbc.dll "${rsodbc_rdata/\"SQLGetDiagField\"/\"SQLGetDiagField\", \"SQLExtra\"}"
expect "an added export fails" 1 \
  "exports: rsodbc.dll: it exports SQLExtra, which expected-exports.txt does not list"
variant rsodbc.dll "${rsodbc_rdata/\"SQLFetch\", /}"
expect "a removed export fails" 1 \
  "exports: rsodbc.dll: expected-exports.txt:$fetch_row lists SQLFetch, which it does not export"
variant rsodbc.dll "${rsodbc_rdata/\"SQLFetch\"/\"sqlfetch\"}"
expect "an export whose case differs fails, as GetProcAddress compares exactly" 1 \
  "exports: rsodbc.dll: it exports sqlfetch, which expected-exports.txt does not list
exports: rsodbc.dll: expected-exports.txt:$fetch_row lists SQLFetch, which it does not export"
variant crypto.dll "$crypto_rdata, \"exported\": [\"SQLExtra\"]" "$crypto_more"
passes "a library's exports are not compared with the driver's list"
# A C++ decorated name can be longer than the 256 bytes a DLL name may take.
long="SQLLong$(printf 'x%.0s' {1..300})"
variant crypto.dll "$crypto_rdata, \"imported_functions\": [\"$long\"]" "$crypto_more"
passes "an imported function's name of 307 bytes passes"
echo "$long" >> "$policy/windows/expected-exports.txt"
variant rsodbc.dll "${rsodbc_rdata/\"SQLFetch\"/\"SQLFetch\", \"$long\"}"
passes "an exported name of 307 bytes passes"
write_policy
grep -v '^SQLFetch$' "$policy/windows/expected-exports.txt" > "$tmp/exports"
cp "$tmp/exports" "$policy/windows/expected-exports.txt"
variant rsodbc.dll "${rsodbc_rdata/\"SQLFetch\", /}"
expect "an expected-exports.txt without an entry point fails, though the exports equal it" 1 \
  "entry-points: expected-exports.txt lacks the interface's entry points SQLFetch"
run "$lib/windows_policy.py" policy "$policy" "$header"
expect "and the policy step fails on it before any file is read" 1 \
  "entry-points: expected-exports.txt lacks the interface's entry points SQLFetch"
write_policy
for flags in 256:0 2304:0 2048:1 768:1 2816:1; do
  variant rsodbc.dll "${rsodbc_rdata/\"load_flags\": 2304/\"load_flags\": ${flags%:*}}"
  hex="$(printf '0x%x' "${flags%:*}")"
  if [[ "${flags#*:}" == 0 ]]; then passes "DependentLoadFlags $hex passes"
  else
    expect "DependentLoadFlags $hex fails" 1 "load-flags: rsodbc.dll: its DependentLoadFlags \
$hex is not LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR with at most LOAD_LIBRARY_SEARCH_SYSTEM32"
  fi
done
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \"ws2_32.dll\"]]" "$crypto_more"
expect "an unlisted run-time DLL name fails" 1 "$(unlisted crypto.dll ws2_32.dll)"
variant crypto.dll "$crypto_rdata, \"strings\": [[\"u\", \"Secur32.DLL\"]]" "$crypto_more"
expect "an unlisted UTF-16LE run-time name fails, ignoring case" 1 \
  "$(unlisted crypto.dll Secur32.DLL)"
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \"dbghelp\"]]" "$crypto_more"
expect "a bare dbghelp the policy does not list for the file fails, as a row names dbghelp.dll" 1 \
  "$(unlisted crypto.dll dbghelp)"
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \"krbcc64\"]]" "$crypto_more"
expect "a bare unshipped DLL name fails" 1 "$(unlisted crypto.dll krbcc64)"
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \"secur32\"]]" "$crypto_more"
expect "a bare name of a system DLL that allowed-imports.txt lists fails" 1 \
  "$(unlisted crypto.dll secur32)"
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \"rsodbc\"]]" "$crypto_more"
expect "a bare name of a set member that nothing imports fails" 1 "$(unlisted crypto.dll rsodbc)"
variant rsodbc.dll "${rsodbc_rdata/\"u\", \"dbghelp.dll\"/\"a\", \" dbghelp.dll\"}"
passes "a run-time name with a leading space meets its row, which cannot hold one"
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \"dbghelp.dll.bak\"],
  [\"a\", \"xdbghelp\"]]" "$crypto_more"
passes "strings that only contain a DLL name or a base name pass"
runtime_rows+=("crypto.dll  netmsg.dll"); write_policy; check
expect "a run-time name row the file no longer holds fails" 1 \
  "runtime-names: crypto.dll: runtime-names.txt:3 lists netmsg.dll, which it does not hold"
runtime_rows=("rsodbc.dll  dbghelp.dll"); write_policy
variant crypto.dll "$crypto_rdata" '"magic": 267'
expect "a PE32 file fails" 1 "image: crypto.dll: a PE32 (32-bit) image, not PE32+"
variant crypto.dll "$crypto_rdata" '"machine": 332'
expect "a file for another machine fails" 1 \
  "image: crypto.dll: built for machine 0x14c, not x86-64 (0x8664)"
printf 'not a PE image\n' > "$tmp/v/crypto.dll"; check "$tmp/v"
expect "a file that is not a PE image fails" 1 "image: crypto.dll: not a PE image (no MZ header)"
variant crypto.dll "$crypto_rdata, \"clr\": true" "$crypto_more"
expect "a .NET image fails" 1 \
  "image: crypto.dll: it has a .NET runtime header, a form this reader does not read"
variant rsodbc.dll "$rsodbc_rdata, \"delay_va\": true"
expect "a delay-import descriptor in the virtual-address form fails" 1 "imports: rsodbc.dll: a \
delay-import descriptor uses virtual addresses (attributes bit 0 is clear), a form this reader \
does not read"

variant rsodbc.dll "$rsodbc_rdata, \"no_name_table\": true"
passes "an import without a name table has its hint/name entries read from its address table"
variant rsodbc.dll "$rsodbc_rdata, \"unused_slots\": 2"
passes "an export address table's unused entries are no exports"
variant rsodbc.dll "$rsodbc_rdata, \"pdb_type\": 13"
expect "a debug record other than CodeView is searched" 1 \
  "$(unlisted rsodbc.dll 'C:\build\crypto.dll')"
variant rsodbc.dll "$rsodbc_rdata, \"debug_size\": 27"
expect "a debug directory that is not whole entries fails" 1 \
  "image: rsodbc.dll: the debug directory's size 27 is not a whole number of entries, at most 65536"
variant rsodbc.dll "$rsodbc_rdata, \"debug_size\": $((28 * 65537))"
expect "a debug directory of more than 65,536 entries fails" 1 \
  "image: rsodbc.dll: the debug directory's size $((28 * 65537)) is not a whole number of entries, \
at most 65536"
# An export with an address and no name is reachable by ordinal, so expected-exports.txt lists it.
variant rsodbc.dll "$rsodbc_rdata, \"functions\": 1"
expect "an export with no name that expected-exports.txt does not list fails" 1 \
  "exports: rsodbc.dll: it exports @$((entry_count + 1)), which expected-exports.txt does not list"
echo "@$((entry_count + 1))" >> "$policy/windows/expected-exports.txt"
check "$tmp/v"; passes "an export with no name passes once expected-exports.txt lists its ordinal"
run "$lib/windows_policy.py" rows "$tmp/v" rsodbc.dll "$policy" expected-exports
if [[ "$status" == 0 ]] && grep -qx "@$((entry_count + 1))" <<<"$out_text"; then pass \
  "rows expected-exports prints the ordinal"
else fail "rows expected-exports prints the ordinal (exit $status: $out_text)"; fi
write_policy
variant rsodbc.dll "$rsodbc_rdata, \"pdb_pointer\": 1000000"
expect "a CodeView record outside the file fails" 1 "image: rsodbc.dll: a CodeView record lies \
outside the file"
# The search skips a CodeView record, so one that is no PDB path's would hide strings.
codeview="image: rsodbc.dll: a CodeView record is not an RSDS record of at most 4121 bytes"
variant rsodbc.dll "$rsodbc_rdata, \"pdb_pointer\": 0"
expect "a CodeView record that does not start with RSDS fails" 1 "$codeview"
variant rsodbc.dll "${rsodbc_rdata/\"pdb\": \"C:/\"pdb\": \"C:$(printf 'p%.0s' {1..4097})}"
expect "a CodeView record over 4,121 bytes fails" 1 "$codeview"
# 24 bytes, then the 19-character path made 4,096 long, and its NUL.
variant rsodbc.dll "${rsodbc_rdata/\"pdb\": \"C:/\"pdb\": \"C:$(printf 'p%.0s' {1..4077})}"
passes "a CodeView record of exactly 4,121 bytes passes"
variant rsodbc.dll "$(rsodbc_spec "[]")"
want=""; for e in $entry_points; do
  want+="${want:+$'\n'}exports: rsodbc.dll: expected-exports.txt:$(grep -nx "$e" \
    "$policy/windows/expected-exports.txt" | cut -d: -f1) lists $e, which it does not export"
done
want="$(LC_ALL=C sort -t' ' -k5,5 <<<"$want")"
expect "a driver with no exported name fails, listing each expected export" 1 "$want"
variant rsodbc.dll '"imports": ["KERNEL32.dll", "crypto.dll", "VCRUNTIME140.dll"],
  "delay_imports": ["Secur32.dll"], "strings": [["u", "dbghelp.dll"]]'
expect "a driver with no export directory fails the same way" 1 "$want"
variant rsodbc.dll "$rsodbc_rdata, \"ordinal_past\": true"
expect "an exported name whose ordinal is past the functions fails" 1 \
  "exports: rsodbc.dll: an exported name's ordinal index $entry_count is past the $entry_count functions"
# The export directory's tables and names fail under exports, as the documentation says.
for table in functions:address names:name; do
  variant rsodbc.dll "$rsodbc_rdata, \"${table%:*}_at\": $((0x7fff0000))"
  expect "an export ${table#*:} table outside every section fails at exports" 1 \
    "exports: rsodbc.dll: the export ${table#*:} table lies outside every section"
done
long_name="$(printf 'F%.0s' {1..4107})"
variant rsodbc.dll "$(rsodbc_spec "${exported%]}, \"$long_name\"]")"
expect "an exported name of 4,107 bytes fails at exports" 1 \
  "exports: rsodbc.dll: an exported name has no NUL within 4096 bytes in its section"
variant rsodbc.dll "$rsodbc_rdata, \"export_name_at\": $((0x7fff0000))"
expect "an export name outside every section fails at exports" 1 \
  "exports: rsodbc.dll: the export name lies outside every section"
variant rsodbc.dll "$rsodbc_rdata, \"load_config_at\": $((0x7fff0000))"
expect "a load configuration outside every section fails at load-flags" 1 \
  "load-flags: rsodbc.dll: the load configuration lies outside every section"
variant rsodbc.dll "$rsodbc_rdata, \"forwarders\": [\"crypto.$(printf 'f%.0s' {1..300})\"]"
expect "a forwarder with no NUL within 256 bytes fails at exports" 1 \
  "exports: rsodbc.dll: a forwarder has no NUL within 256 bytes in its section"
variant rsodbc.dll "${rsodbc_rdata/\"load_flags\"/\"functions\": 65510, \"load_flags\"}"
expect "an export directory of more than 65,536 functions fails" 1 \
  "exports: rsodbc.dll: the export directory lists more than 65536 functions or names"
variant rsodbc.dll "${rsodbc_rdata/\"load_flags\"/\"names_count\": 65537, \"load_flags\"}"
expect "an export directory of more than 65,536 names fails" 1 \
  "exports: rsodbc.dll: the export directory lists more than 65536 functions or names"
# A library's forwarders are read from the same table, whose names the driver's rule never reads.
variant crypto.dll '"imports": ["KERNEL32.dll", "msvcr100.dll"], "export_name": "crypto.dll", "functions": 65537' \
  "$crypto_more"
expect "a library's export directory of more than 65,536 functions fails" 1 \
  "exports: crypto.dll: the export directory lists more than 65536 functions or names"
variant crypto.dll '"imports": ["KERNEL32.dll", "msvcr100.dll"], "export_name": "crypto.dll", "functions": 65536' \
  "$crypto_more"
passes "a library's export directory of exactly 65,536 functions passes"
python3 -I -c 'import struct, sys
key = "NOT_VERSION\0".encode("utf-16-le")
open(sys.argv[1], "wb").write(struct.pack("<HHH", 6 + len(key), 0, 1) + key)' "$tmp/other.bin"
# A string whose length leaves out its padding, as a linker writes it: the next one is aligned.
variant crypto.dll "$crypto_rdata" '"version": {"Comments": "ab", "OriginalFilename": "crypto32.dll"},
  "version_unpadded": true, "certificate": "\u0000signed by msvcr100.dll\u0000"'
if [[ "$status" == 0 && "$out_text" == *"run-time names, OriginalFilename crypto32.dll"* ]]; then
  pass "a version string after one whose length leaves out its padding is read at the next 32 bits"
else fail "a version string after one whose length leaves out its padding is read at the next 32 bits (exit $status: $out_text)"; fi
variant crypto.dll "$crypto_rdata" "\"resources\": [[16, 1, 1033, \"@$tmp/other.bin\"]]"
expect "a version resource that is not a VS_VERSIONINFO fails" 1 \
  "version: crypto.dll: a version resource is 'NOT_VERSION', not a VS_VERSIONINFO"
variant crypto.dll "$crypto_rdata" '"resources": [[16, 1, 1033, "ab"]]'
expect "a version resource shorter than a node fails" 1 \
  "version: crypto.dll: a version resource node lies outside its parent"
python3 -I -c 'import struct, sys
key = "VS_VERSION_INFO".encode("utf-16-le")
open(sys.argv[1], "wb").write(struct.pack("<HHH", 6 + len(key), 0, 1) + key)' "$tmp/no-nul.bin"
variant crypto.dll "$crypto_rdata" "\"resources\": [[16, 1, 1033, \"@$tmp/no-nul.bin\"]]"
expect "a version resource key without a NUL fails" 1 \
  "version: crypto.dll: a version resource node's key has no NUL"
variant crypto.dll "$crypto_rdata" '"resources": [[16, 1, 1033, "version"]]'
expect "a version resource whose length runs past it fails" 1 \
  "version: crypto.dll: a version resource node's length 25974 runs past its parent"
runtime_rows+=("crypto.dll  .dll"); write_policy
variant crypto.dll "$crypto_rdata, \"strings\": [[\"a\", \".dll\"], [\"a\", \"   \"]]" \
  "$crypto_more"
passes "a .dll string alone is a run-time name, and names no base name a blank string equals"
runtime_rows=("rsodbc.dll  dbghelp.dll"); write_policy
rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"; ln -s crypto.dll "$tmp/v/link.dll"; check "$tmp/v"
expect "a link in the set fails" 1 "set: $tmp/v/link.dll is not a regular file"
rm "$tmp/v/link.dll" "$tmp/v/rsodbc.dll"; check "$tmp/v"
expect "a set without the driver fails" 1 "set: $tmp/v holds no rsodbc.dll"
rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"; mkfifo "$tmp/v/pipe.dll"; check "$tmp/v"
expect "a FIFO in the set fails before it is opened" 1 "set: $tmp/v/pipe.dll is not a regular file"
rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"; chmod 000 "$tmp/v/crypto.dll"
if [[ -r "$tmp/v/crypto.dll" ]]; then # root reads it anyway
  skip "an unreadable set file fails" "root reads a file of mode 000"
else check "$tmp/v"
  expect "an unreadable set file fails" 1 "set: cannot read $tmp/v/crypto.dll: Permission denied"
fi
chmod 644 "$tmp/v/crypto.dll"
# The import name table's bounds, on entries make_pe.py writes as given.
variant crypto.dll "$crypto_rdata, \"imported_ordinals\": 65536" "$crypto_more"
expect "an import name table of more than 65,536 entries fails" 1 \
  "imports: crypto.dll: an import name table has more than 65536 entries"
variant crypto.dll "$crypto_rdata, \"raw_thunks\": [2147483648]" "$crypto_more"
expect "an import name table entry at 2^31 without the ordinal bit fails" 1 \
  "imports: crypto.dll: an import name table entry 0x80000000 is neither an ordinal nor a hint/name address"
# The version resource's value bound, and only StringFileInfo read for strings.
python3 -I -c 'import struct, sys
key = "VS_VERSION_INFO\0".encode("utf-16-le")
open(sys.argv[1], "wb").write(struct.pack("<HHH", 44, 1000, 0) + key + bytes(6))' "$tmp/long-value.bin"
variant crypto.dll "$crypto_rdata" "\"resources\": [[16, 1, 1033, \"@$tmp/long-value.bin\"]]"
expect "a version resource value that runs past its node fails" 1 \
  "version: crypto.dll: VS_VERSION_INFO's value runs past its node"
python3 -I -c 'import struct, sys
def node(key, value=b"", text=1, children=()):
    out = struct.pack("<HHH", 0, len(value) // 2 if text else len(value), text) + (key + "\0").encode("utf-16-le")
    out += bytes(-len(out) % 4) + value
    for child in children:
        out += bytes(-len(out) % 4) + child
    return struct.pack("<H", len(out)) + out[2:]
name = node("OriginalFilename", "evil.dll\0".encode("utf-16-le"))
var = node("Translation", b"\x09\x04\xb0\x04", 0, [name])
open(sys.argv[1], "wb").write(node("VS_VERSION_INFO", children=[node("VarFileInfo", children=[var])]))' \
  "$tmp/var.bin"
variant crypto.dll "$crypto_rdata" "\"resources\": [[16, 1, 1033, \"@$tmp/var.bin\"]]"
if [[ "$status" == 0 && "$out_text" == *"crypto.dll: 2 imports, 0 delay imports, 0 forwarded DLLs, 0 run-time names, OriginalFilename none"* ]]
then pass "a VarFileInfo's nodes are not read as strings, even with children"
else fail "a VarFileInfo's nodes are not read as strings, even with children (exit $status: $out_text)"; fi
runtime_rows+=("rsodbc.dll  two words"); write_policy
run "$lib/windows_policy.py" policy "$policy" "$header"
expect "a runtime-names.txt string may hold a space, which a DLL name may not" 0 \
  "the Windows policy files are well formed, and expected-exports.txt holds every entry point"
runtime_rows=("rsodbc.dll  dbghelp.dll"); write_policy
run "$lib/windows_policy.py" policy "$policy" "$tmp/no-header.hpp"
expect "an unreadable interface header fails at entry-points" 1 \
  "entry-points: cannot read the interface header $tmp/no-header.hpp: [Errno 2] No such file or \
directory: '$tmp/no-header.hpp'"
# rows grows the base names from the strings it finds: crypto.dll's bare netapi32 is a run-time
# name only because rsodbc.dll holds netapi32.dll.
dll "$tmp/grow" rsodbc.dll '"export_name": "rsodbc.dll", "strings": [["a", "netapi32.dll"]]'
dll "$tmp/grow" crypto.dll '"strings": [["a", "netapi32"]]'
run "$lib/windows_policy.py" rows "$tmp/grow" rsodbc.dll "$policy" runtime-names
expect "rows finds a bare name that another file's .dll string names" 0 \
  "crypto.dll  netapi32
rsodbc.dll  netapi32.dll"

# The search's edges: import names alone, and ssl.dll's base name.
plain() { # plain [<more rdata>]: a driver importing ssl.dll's SSL_CTX_new and holding sslmode
  dll "$tmp/plain" rsodbc.dll "\"imports\": [\"KERNEL32.dll\", \"ssl.dll\"],
    \"imported_functions\": [\"SSL_CTX_new\"], \"export_name\": \"rsodbc.dll\",
    \"exported\": $exported, \"strings\": [[\"a\", \"sslmode\"]${1:+, $1}]"
}
plain
dll "$tmp/plain" ssl.dll '"imports": ["KERNEL32.dll"], "export_name": "ssl.dll", "functions": 1'
imports_rows=("rsodbc.dll  KERNEL32.dll" "rsodbc.dll  ssl.dll" "ssl.dll  KERNEL32.dll")
runtime_rows=(); write_policy
check "$tmp/plain"
expect "a set whose only .dll strings are import names passes with an empty runtime-names.txt, and \
sslmode and an imported SSL_CTX_new are no ssl hit" 0 \
  "the set's 2 files meet the Windows loading policy
rsodbc.dll: 2 imports, 0 delay imports, 0 forwarded DLLs, 0 run-time names, OriginalFilename none
ssl.dll: 1 imports, 0 delay imports, 0 forwarded DLLs, 0 run-time names, OriginalFilename none"
plain '["a", "ssl"]'; check "$tmp/plain"
expect "a whole NUL-terminated ssl string is a hit" 1 "$(unlisted rsodbc.dll ssl)"
plain; imports_rows+=("other.dll  KERNEL32.dll"); write_policy; check "$tmp/plain"
expect "a row naming a file the set does not hold fails" 1 \
  "policy: allowed-imports.txt names other.dll, which is not a file of the set"

# Malformed policy files fail before any image is read.
malformed() { # malformed <file> <row> <message>
  write_policy; echo "$2" >> "$policy/windows/$1"
  run "$lib/windows_policy.py" policy "$policy" "$header"
  expect "a malformed $1 row fails: $2" 1 "$3"
}
imports_rows=("rsodbc.dll  KERNEL32.dll")
row_is="policy: allowed-imports.txt:3: a row is <file> <dll>"
malformed allowed-imports.txt "rsodbc.dll" "$row_is"
malformed allowed-imports.txt "rsodbc.dll  C:\\x.dll" "$row_is"
malformed allowed-imports.txt "RSODBC.dll  kernel32.DLL" \
  "policy: allowed-imports.txt:3: kernel32.DLL is listed twice for RSODBC.dll"
malformed runtime-names.txt "rsodbc.dll" "policy: runtime-names.txt:2: a row is <file> <string>"
row_is="policy: expected-exports.txt:$((entry_count + 2)): a row is one exported name, listed once"
malformed expected-exports.txt "SQLFetch" "$row_is"
malformed expected-exports.txt "SQL Fetch" "$row_is"
malformed allowed-imports.txt "rsodbc.dll/x  KERNEL32.dll" \
  "policy: allowed-imports.txt:3: a row is <file> <dll>"
write_policy; printf 'rsodbc.dll  KERNEL32.dll  \xe9\n' >> "$policy/windows/allowed-imports.txt"
run "$lib/windows_policy.py" policy "$policy" "$header"
if [[ "$status" == 1 && "$out_text" == \
  "policy: cannot read $policy/windows/allowed-imports.txt: 'ascii' codec"* ]]
then pass "a policy file with a byte outside ASCII fails"
else fail "a policy file with a byte outside ASCII fails ($status: $out_text)"; fi
imports_rows=("${good_imports[@]}"); runtime_rows=("rsodbc.dll  dbghelp.dll" "other.dll  x.dll"); \
  write_policy; check
expect "a runtime-names.txt row for a file the set does not hold fails" 1 \
  "policy: runtime-names.txt names other.dll, which is not a file of the set"
runtime_rows=("rsodbc.dll  dbghelp.dll"); write_policy
# The rename's tools refuse an unshipped name with .dll, which the name search would never match.
echo "leashw64.dll" >> "$policy/windows/unshipped-names.txt"
run "$lib/windows_policy.py" policy "$policy" "$header"
expect "an unshipped-names.txt row that is not a lower-case base name fails" 1 \
  "policy: unshipped-names.txt:$(wc -l < "$policy/windows/unshipped-names.txt" | tr -d ' '): \
'leashw64.dll' is not a lower-case base name"
cp "$odbc/policy/windows/unshipped-names.txt" "$policy/windows/"
run "$lib/windows_policy.py" rows "$tmp/set" rsodbc.dll "$policy" policy
usage="usage: windows_policy.py policy <policy-dir> <interface-header> | check <set-dir> <driver>"
usage+=" <policy-dir> <interface-header> | rows <set-dir> <driver> <policy-dir>"
usage+=" <allowed-imports|expected-exports|runtime-names>"
expect "rows of an unknown kind is a usage error" 2 "$usage"
# The set's own refusals: a name lock.py refuses, a file over the reader's bound, a folder it
# cannot list.
rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"; printf 'x' > "$tmp/v/a b.dll"; check "$tmp/v"
if [[ "$status" == 1 && "$out_text" == "set: "* && "$out_text" != *$'\n'* ]]; then
  pass "a set file whose name lock.py refuses fails at the set rule"
else fail "a set file whose name lock.py refuses fails at the set rule ($status: $out_text)"; fi
# The sparse file is made with Python, as macOS has no truncate(1).
rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"
python3 -I -c 'import sys; open(sys.argv[1], "wb").truncate((1 << 30) + 1)' "$tmp/v/big.dll"
check "$tmp/v"
expect "a set file over 1 GiB fails before it is read" 1 \
  "image: big.dll: the file is larger than 1 GiB"
rm -rf -- "${tmp:?}/v"; good_set "$tmp/v"; chmod 000 "$tmp/v"
if [[ -r "$tmp/v" ]]; then skip "an unreadable set folder fails in one line" "running as root"
else
  check "$tmp/v"
  if [[ "$status" == 1 && "$out_text" == "set: cannot read $tmp/v: Permission denied" ]]
  then pass "an unreadable set folder fails in one line"
  else fail "an unreadable set folder fails in one line ($status: $out_text)"; fi
fi
chmod 755 "$tmp/v"
write_policy; rm "$policy/windows/runtime-names.txt"
run "$lib/windows_policy.py" policy "$policy" "$header"
if [[ "$status" == 1 && "$out_text" == "policy: cannot read $policy/windows/runtime-names.txt:"* ]]
then pass "a missing policy file fails"
else fail "a missing policy file fails ($status: $out_text)"; fi
write_policy

# The environment-name rule on a set, as windows_amd64: every file is scanned.
envpol="$tmp/envpolicy"; mkdir -p "$envpol"
echo "SSL" > "$envpol/environment-prefixes.txt"
for f in refused-environment known-environment ignored-strings; do : > "$envpol/$f.txt"; done
dll "$tmp/envset" rsodbc.dll '"strings": [["a", "SSL_CERT_FILE"]]'
dll "$tmp/envset" crypto.dll '"strings": [["a", "SSL_CERT_FILE"], ["a", "SSLKEYLOGFILE"]]'
echo "SSL_CERT_FILE  load" > "$envpol/refused-environment.txt"
run "$lib/env_names.py" "$envpol" "$tmp/envset" windows_amd64
expect "an added environment name in one file of the set fails, naming the file" 1 \
  "environment: the set's crypto.dll names SSLKEYLOGFILE, which no policy list classifies; add it \
to refused-environment.txt or known-environment.txt, or, if it is not a variable, to \
ignored-strings.txt"
# The driver sorts after its libraries in the real set, so the scan must not stop at the first.
dll "$tmp/envlast" crypto.dll '"strings": [["a", "SSL_CERT_FILE"]]'
dll "$tmp/envlast" rsodbc.dll '"strings": [["a", "SSLRS_ONLY"]]'
run "$lib/env_names.py" "$envpol" "$tmp/envlast" windows_amd64
expect "an added environment name held only by the file that sorts last fails, naming it" 1 \
  "environment: the set's rsodbc.dll names SSLRS_ONLY, which no policy list classifies; add it to \
refused-environment.txt or known-environment.txt, or, if it is not a variable, to \
ignored-strings.txt"
echo "SSLKEYLOGFILE  load  # only in windows_amd64" >> "$envpol/refused-environment.txt"
classified="the set's 2 scanned names are all classified (2 refused, 0 known, 0 ignored)"
run "$lib/env_names.py" "$envpol" "$tmp/envset" windows_amd64
expect "the set passes once the name is classified, in a row marked only in windows_amd64" 0 \
  "$classified"
echo "SSLX_ONLY  -  a test row  # only in windows_amd64" > "$envpol/known-environment.txt"
run "$lib/env_names.py" "$envpol" "$tmp/envset" linux_amd64
expect "a row only in windows_amd64 is not stale on another platform" 0 "$classified"
run "$lib/env_names.py" "$envpol" "$tmp/envset" windows_amd64
expect "and is stale on windows_amd64 when no file of the set holds it" 1 \
  "environment: SSLX_ONLY (known-environment.txt:1) is no longer in the set; remove its row"
: > "$envpol/known-environment.txt"
echo "SSL_GONE_SERVICE  load  # built at run time from SSL_GONE_" >> "$envpol/refused-environment.txt"
run "$lib/env_names.py" "$envpol" "$tmp/envset" windows_amd64
expect "a row built from a string no file of the set holds is stale, naming the set" 1 \
  "environment: SSL_GONE_SERVICE (refused-environment.txt:3) is built from SSL_GONE_, which is no longer in the set"
echo "SSLKEYLOGFILE  load" > "$envpol/refused-environment.txt"
echo "SSL_CERT_FILE  -  a test row  # only in osx_arm64" > "$envpol/known-environment.txt"
run "$lib/env_names.py" "$envpol" "$tmp/envset" windows_amd64
expect "a name the set holds, whose row is only in another platform, fails, naming the files" 1 \
  "environment: the set's crypto.dll,rsodbc.dll names SSL_CERT_FILE, whose row \
(known-environment.txt:1) is only in osx_arm64; add windows_amd64 to the row if this driver holds \
it for the same reason"
mkdir "$tmp/envset/sub"
run "$lib/env_names.py" "$envpol" "$tmp/envset" windows_amd64
expect "a set with a directory in it fails" 1 \
  "environment: cannot read $tmp/envset/sub: not a regular file"

# The locked 2.3.0.0 DLLs with the checked-in policy, offline: from the MSI already in the release
# cache or the mirror, taken out with msi.py, which verification uses.
lock="$(bash "$odbc/tools/check-lock.sh" "${RS_DRIVER_LOCK:-$odbc/driver.lock}")"
row="$(awk '$1 == "driver" && $2 == "windows_amd64"' <<<"$lock")"
read -r _ _ driver _ _ url msi_sha _ <<<"${row:-x x x x x x x}"
msi=""
for c in "${RS_DRIVER_MIRROR:-/nonexistent}/$msi_sha" "$release_cache_dir/$msi_sha"; do
  if [[ -n "$row" && -f "$c" && "$(sha "$c")" == "$msi_sha" ]]; then msi="$c"; break; fi
done
real="the locked Windows DLLs pass the checked-in policy"
real_env="the locked Windows DLLs pass the environment-name rule"
if [[ -z "$row" ]]; then
  skip "$real" "the lock under test has no windows_amd64 row"; skip "$real_env" "the same"
elif [[ -z "$msi" ]]; then
  skip "$real" "the MSI $msi_sha is in neither the release cache nor RS_DRIVER_MIRROR"
  skip "$real_env" "the same"
else
  mkdir "$tmp/real"
  version="$(awk '$1 == "version" { print $2 }' <<<"$lock")"
  run "$lib/msi.py" check "$msi" "$odbc/policy" "$version" "$url" "$msi_sha" "$tmp/real"
  [[ "$status" == 0 ]] || fail "msi.py takes the locked set out of the MSI ($out_text)"
  run "$lib/windows_policy.py" check "$tmp/real" "$driver" "$odbc/policy" "$header"
  if [[ "$status" == 0 ]]; then pass "$real"; else fail "$real ($out_text)"; fi
  run "$lib/env_names.py" "$odbc/policy" "$tmp/real" windows_amd64
  if [[ "$status" == 0 ]]; then pass "$real_env"; else fail "$real_env ($out_text)"; fi
fi

echo "# $n cases, $((n - failed)) passed"
exit $((failed > 0))
