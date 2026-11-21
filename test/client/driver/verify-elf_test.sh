#!/usr/bin/env bash
# Tool tests for the Linux loading policy, tools/lib/elf_policy.sh: small shared
# libraries built at test time with the flags that break each rule, and copies of the real
# drivers with one field changed (patch_elf.py), each fail at their rule; the unmodified drivers
# pass the checked-in policy. Run by run-tool-tests.sh. The libraries are C, built with gcc; only
# the expansion of the interface header's entry points needs gcc's C++ front end (g++).
#
# The real drivers are taken from the packages of the lock under test in the release cache
# (release-cache.sh), filled from RS_DRIVER_MIRROR or by one download each.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
odbc="$repo/third_party/redshift_odbc"
# ELF_POLICY_TOOL runs the cases against another copy of elf_policy.sh, with common.sh beside it,
# such as a mutant in a mutation check; by default, the tree's.
policy_tool="${ELF_POLICY_TOOL:-$odbc/tools/lib/elf_policy.sh}"
header="$repo/src/transport/odbc_driver.hpp"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=release-cache.sh
source "$here/release-cache.sh"
require_tools python3 readelf od objcopy

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# check <name> <policy-dir> <platform> <driver> <expected-exit> <substring> [<header>]
check() {
  local out status
  if [[ ! -f "$4" ]]; then fail "$1 (the input $4 was not built)"; return; fi
  out="$(bash "$policy_tool" "$2" "${7:-$header}" "$3" "$4" 2>&1)" && status=0 || status=$?
  # A failure must be the run's last line: the rule stopped the run.
  if [[ "$status" == "$5" && "$out" == *"$6"* ]] && { [[ "$5" != 1 ]] || [[ "${out##*$'\n'}" == *"$6"* ]]; }; then
    pass "$1"; else fail "$1 (exit $status, wanted $5; output: $out)"; fi
}
# patch <in> <out> <change>...: a copy with one field changed. A change the input does not have
# (another linker wrote the fixture differently) is a named failed case, not an abort, so the cases
# after it still run.
patch() {
  local out
  out="$(python3 -I "$here/patch_elf.py" "$@" 2>&1)" ||
    fail "patching $(basename "$1") into $(basename "$2") failed: $out"
}

# The libraries built below need a Linux compiler for the host's own architecture (elsewhere, macOS
# say, they skip); the real drivers and their patched copies are only read, so their cases run on any host.
host=""
case "$(uname -s)/$(uname -m)" in
Linux/x86_64) host=linux_amd64 ;;
Linux/aarch64) host=linux_arm64 ;;
esac

# The real drivers, from the lock's packages.
base_lock="${RS_DRIVER_LOCK:-$odbc/driver.lock}"
[[ "$base_lock" == /* ]] || base_lock="$PWD/$base_lock"
locked_drivers "$base_lock" "$tmp" > /dev/null # $tmp/<platform>.so per Linux row (release-cache.sh)
real="$odbc/policy/linux"
for p in linux_amd64 linux_arm64; do
  if [[ -f "$tmp/$p.so" ]]; then
    # The newest version is named, which is the highest number, not the last one read: the driver
    # needs GLIBC_2.3.4 and GLIBC_2.25, which sort the other way round.
    check "the unmodified $p driver fits the checked-in policy" "$real" "$p" "$tmp/$p.so" 0 "fits the Linux policy (newest glibc version required: 2.25)"
  else echo "ok - $p # SKIP the lock under test has no $p row"; fi
done
# The copies below are of the amd64 driver whatever the host (elf_policy.sh reads either
# architecture), as its needs include GLIBC_2.3.4 and GLIBC_2.25, which only a number-by-number
# comparison orders correctly.
drv="$tmp/linux_amd64.so"
[[ -f "$drv" ]] || drv="$tmp/linux_arm64.so"
dp=linux_amd64; [[ "$drv" == *linux_amd64.so ]] || dp=linux_arm64
op=linux_arm64; [[ "$dp" == linux_amd64 ]] || op=linux_amd64

patch "$drv" "$tmp/machine.so" byte 18 "$([[ "$dp" == linux_amd64 ]] && echo 183 || echo 62)" byte 19 0
check "a copy of the driver with another ELF machine fails" "$real" "$dp" "$tmp/machine.so" 1 "machine: the driver is ELF machine"
check "the driver of one architecture fails for the other" "$real" "$op" "$drv" 1 "machine: the driver is ELF machine"
patch "$drv" "$tmp/class32.so" byte 4 1
check "a copy marked 32-bit fails" "$real" "$dp" "$tmp/class32.so" 1 "machine: the driver is not a 64-bit little-endian ELF file"
patch "$drv" "$tmp/bigendian.so" byte 5 2
check "a copy marked big-endian fails" "$real" "$dp" "$tmp/bigendian.so" 1 "machine: the driver is not a 64-bit little-endian ELF file"
# The rules read the program headers through readelf, which must count them as glibc does. With
# e_phnum PN_XNUM (0xffff), readelf takes the count from section 0's sh_info, at offset 44 of the
# section header table (e_shoff), where this copy keeps the real count, and glibc reads 65535
# headers; and glibc refuses an e_phentsize other than 56. The e_phentsize copy changes only its
# high byte (56 becomes 312), so a check of the low byte alone would pass it.
shoff="$(readelf -hW "$drv" | awk '/Start of section headers/ { print $5 }')"
phnum="$(readelf -hW "$drv" | awk '/Number of program headers/ { print $5 }')"
patch "$drv" "$tmp/xnum.so" byte 0x38 0xff byte 0x39 0xff byte $((shoff + 44)) "$phnum"
check "a copy whose e_phnum is PN_XNUM fails" "$real" "$dp" "$tmp/xnum.so" 1 "machine: the driver's e_phnum is PN_XNUM (0xffff)"
patch "$drv" "$tmp/phentsize.so" byte 0x37 1
check "a copy whose e_phentsize is 312, its high byte changed, fails" "$real" "$dp" "$tmp/phentsize.so" 1 \
  "machine: the driver's program headers are 312 bytes each (e_phentsize), not the 56 glibc reads"
# A version with a leading zero is still compared as a number: 2.08 is 2.8, older than the floor,
# and the newest version named is the driver's 2.18, not the 2.08 an octal reading would take.
patch "$drv" "$tmp/zero.so" replace GLIBC_2.25 GLIBC_2.08
check "a copy that requires GLIBC_2.08 is accepted, 08 read as 8" "$real" "$dp" "$tmp/zero.so" 0 \
  "fits the Linux policy (newest glibc version required: 2.18)"
patch "$drv" "$tmp/glibc.so" replace GLIBC_2.25 GLIBC_2.99
check "a copy that requires GLIBC_2.99 fails" "$real" "$dp" "$tmp/glibc.so" 1 "glibc: the driver requires GLIBC_2.99, newer than the floor 2.28"
patch "$drv" "$tmp/private.so" replace GLIBC_2.25 GLIBC_PRIV
check "a copy that requires a non-numeric glibc version fails" "$real" "$dp" "$tmp/private.so" 1 "glibc: the driver requires GLIBC_PRIV, which is not a released"
mkdir -p "$tmp/GLIBC_dir"; cp "$drv" "$tmp/GLIBC_dir/driver.so"
check "a GLIBC_ in the driver's path is not read as a need" "$real" "$dp" "$tmp/GLIBC_dir/driver.so" 0 "fits"
# readelf -V starts the needs at .gnu.version_r's section-header offset. Moving that header past
# the file, 8 bytes in, or to the last file makes readelf list fewer files than DT_VERNEEDNUM, so
# each fails. This does not compare the section-header offset with DT_VERNEED; that signed-file
# disagreement remains an accepted residual.
last="$(readelf -VW "$drv" | awk '$4 == "File:" { o = $1 } END { sub(/:$/, "", o); print o }')"
for c in "past the end of the file:0x10000000:0" "8 bytes in:8:1" "to the last file of needs:$last:1"; do
  IFS=: read -r what by files <<<"$c"
  rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" shdr-add .gnu.version_r offset "$by"
  check "a copy whose .gnu.version_r header is moved $what fails" "$real" "$dp" "$tmp/needs.so" 1 \
    "glibc: readelf -V reads $files version-need entries, where DT_VERNEEDNUM is"
done
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" dyn-tag 0x6fffffff 0x15 shdr-add .gnu.version_r offset 0x10000000
check "a copy with no DT_VERNEEDNUM whose needs readelf cannot read fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "glibc: readelf -V reads 0 version-need entries, where DT_VERNEEDNUM is 0 and DT_VERNEED is set"
# readelf stops after .gnu.version_r's sh_info files and each file's vn_cnt needs, while the policy
# walks vn_aux, vn_next and vna_next from that section-header offset. The printed names must cover
# that chain.
vr_off="$(readelf -SW "$drv" | awk '{ sub(/^ *\[ *[0-9]+\] +/, "") } $1 == ".gnu.version_r" { print "0x" $4 }')"
vr_size="$(readelf -SW "$drv" | awk '{ sub(/^ *\[ *[0-9]+\] +/, "") } $1 == ".gnu.version_r" { print "0x" $5 }')"
str_index="$(readelf -SW "$drv" | awk '{ sub(/^ *\[ */, "") } $2 == ".dynstr" { print $1 + 0 }')"
read -r libc libc_needs <<<"$(readelf -VW "$drv" | awk '$4 == "File:" && $5 == "libc.so.6" { sub(/:$/, "", $1); print $1, $7 }')"
pthread_needs="$(readelf -VW "$drv" | awk '$4 == "File:" && $5 == "libpthread.so.0" { print $7 }')"
need="$(readelf -VW "$drv" | awk '$2 == "Name:" && $3 == "GLIBC_2.25" { sub(/:$/, "", $1); print $1; exit }')"
num="$(readelf -dW "$drv" | awk '$2 == "(VERNEEDNUM)" { print $3 }')"
# shdr_at <file> <section>: the file offset of the section's header, 64 bytes at e_shoff each;
# sh_name is its first 4 bytes.
shdr_at() {
  local at; at="$(readelf -hW "$1" | awk '/Start of section headers/ { print $5 }')"
  echo $((at + 64 * $(readelf -SW "$1" | awk -v s="$2" '{ sub(/^ *\[ */, "") } $2 == s { print $1 + 0 }')))
}
# sh_name <file> <section>
sh_name() { od -An -tu4 -j "$(shdr_at "$1" "$2")" -N4 -- "$1" | tr -d ' '; }
total="$(readelf -VW "$drv" | awk '$2 == "Name:"' | wc -l | tr -d ' ')"
cn="glibc: readelf -V prints" chain="version-need names, where the chain read from .gnu.version_r's section-header offset holds $total needs"
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" dyn-tag 0x6ffffffe 0x15 dyn-val 0x6fffffff $((num - 1))
check "a copy with no DT_VERNEED and a mismatching DT_VERNEEDNUM fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "DT_VERNEEDNUM is $((num - 1)) and DT_VERNEED is absent"
# Both bounds in the walk have their own cases: vn_aux starts a file's auxiliary chain, while
# vna_next advances within one. Each pointer is made to leave the section.
rm -f "$tmp/needs.so"; at=$((vr_off + libc + 8)) # vn_aux
patch "$drv" "$tmp/needs.so" dyn-val 0x6fffffff $((num - 2)) shdr-set .gnu.version_r info $((num - 2)) u32 "$at" 0xffffffff
check "a copy whose vn_aux points past .gnu.version_r fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "glibc: a version-need auxiliary chain does not end inside the .gnu.version_r section"
# readelf stops after sh_info files, while the policy follows the last file's nonzero vn_next.
# Each next pointer is made to leave the section, to land exactly at its end, and to start an entry
# 15 bytes before its end, so that the 16-byte entry crosses it by one: these cases catch a walk
# bounded by less than a whole entry, or only by the 1 MiB limit. The vna_next is the GLIBC_2.25
# need's, at $need; the vn_next is the last file's, at $last. Each case gives the pointer either as
# "value:<n>", written as it is, or as "before-end:<n>", the distance from the section's end to
# where the next entry starts, from which the relative pointer is worked out.
for c in "past .gnu.version_r:value:0xffffffff" "to the end of .gnu.version_r:before-end:0" \
  "across the end of .gnu.version_r:before-end:15"; do
  IFS=: read -r where kind amount <<<"$c"
  for p in "vna_next:$need:a version-need auxiliary chain" "last vn_next:$last:the version-need file chain"; do
    IFS=: read -r field from message <<<"$p"
    by="$amount"; [[ "$kind" == value ]] || by=$((vr_size - amount - from))
    rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" u32 $((vr_off + from + 12)) "$by"
    check "a copy whose $field points $where fails" "$real" "$dp" "$tmp/needs.so" 1 \
      "glibc: $message does not end inside the .gnu.version_r section"
  done
done
# Lowering libc's vn_cnt hides GLIBC_2.99, and lowering .gnu.version_r's sh_info omits
# libpthread. A second, flagless VERNEED header points at libc: readelf still includes its files
# and names, making the aggregate counts equal the full first chain. The heading-count rule must
# refuse that section before the hidden requirement can pass as 2.18.
# The compensating header starts at libc, or at an earlier file when the needs it must make up are
# odd, and lists that file through libc, so those files are shown twice: libc's lowered vn_cnt k and
# the needs S of the files before it make up libc's and libpthread's, 2k + S = libc_needs +
# pthread_needs (2.3.0.0's sum was even, so S = 0; 2.3.1.0's x86_64 library has 15 + 4, so the
# header starts at libm). The copy must list as many needs as the driver, or the case tests less.
start="$libc" s_needs=0 s_files=1 hidden=$((libc_needs + pthread_needs))
while read -r at needs; do
  ((hidden % 2 == 0)) && break
  start="$at" s_needs=$((s_needs + needs)) s_files=$((s_files + 1)) hidden=$((libc_needs + pthread_needs - s_needs))
done < <(readelf -VW "$drv" | awk '$4 == "File:" { sub(/:$/, "", $1); print $1, $7 }' |
  awk -v libc="$libc" '$1 == libc { stop = 1 } !stop' | tac)
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" \
  byte $((vr_off + libc + 2)) $((hidden / 2)) byte $((vr_off + libc + 3)) 0 \
  replace GLIBC_2.25 GLIBC_2.99 shdr-set .gnu.version_r info $((num - 1)) \
  shdr-set .note.gnu.build-id type 0x6ffffffe shdr-set .note.gnu.build-id flags 0 \
  shdr-set .note.gnu.build-id offset $((vr_off + start)) \
  shdr-set .note.gnu.build-id size $((vr_size - start)) shdr-set .note.gnu.build-id link "$str_index" \
  shdr-set .note.gnu.build-id info "$s_files"
listed() { readelf -VW "$1" 2>/dev/null | awk '$4 == "File:" { n += $7 } END { print n + 0 }'; }
[[ "$hidden" -gt 0 && $((hidden % 2)) == 0 && "$(listed "$tmp/needs.so")" == "$(listed "$drv")" ]] &&
  pass "the compensating-VERNEED copy lists as many needs as the driver ($(listed "$drv"))" ||
  fail "the compensating-VERNEED copy lists $(listed "$tmp/needs.so") needs, not the driver's $(listed "$drv")"
check "a flagless second VERNEED section that compensates for hidden needs fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "glibc: readelf -V must list exactly one version-needs section, named .gnu.version_r"
# readelf -d and -V read names from the first STRTAB section named .dynstr, flags or not, which
# section() does not list without a Flg column: a flagless one before the real .dynstr, here the
# build-id note retyped and renamed (readelf reports multiple dynamic string tables). And one where
# that flagless .dynstr is the only exact name: the real one renamed ".dynstr ", which readelf -SW
# prints alike, and .comment, renamed ".dynstr " to supply that name, made flagless too.
str_name="$(sh_name "$drv" .dynstr)"
note_index="$(readelf -SW "$drv" | awk '{ sub(/^ *\[ */, "") } $2 == ".note.gnu.build-id" { print $1 + 0 }')"
read -r ds_off ds_size <<<"$(readelf -SW "$drv" | awk '{ sub(/^ *\[ *[0-9]+\] +/, "") } $1 == ".dynstr" { print "0x" $4, "0x" $5 }')"
flagless_dynstr=(shdr-set .note.gnu.build-id type 3 shdr-set .note.gnu.build-id flags 0
  shdr-set .note.gnu.build-id offset "$ds_off" shdr-set .note.gnu.build-id size "$ds_size")
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" "${flagless_dynstr[@]}" shdr-set .note.gnu.build-id name "$str_name"
check "a flagless second .dynstr section fails at strings" "$real" "$dp" "$tmp/needs.so" 1 \
  "strings: the sections named exactly .dynstr are $note_index $str_index, not only the checked row $str_index"
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" "${flagless_dynstr[@]}" replace .comment ".dynstr " \
  shdr-set ".dynstr " flags 0 shdr-set .dynstr name "$(sh_name "$drv" .comment)" \
  shdr-set .note.gnu.build-id name "$str_name"
check "a checked .dynstr named '.dynstr ' beside a flagless .dynstr fails at strings" "$real" "$dp" "$tmp/needs.so" 1 \
  "strings: the sections named exactly .dynstr are $note_index, not only the checked row $str_index"
# The version-needs heading must have the exact name: a space keeps readelf -V listing its needs.
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" replace .gnu.version_r ".gnu.version r"
check "a version-needs heading named '.gnu.version r' fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "glibc: readelf -V must list exactly one version-needs section, named .gnu.version_r"
# The one heading has an exact name, but no exact-name ten-field row: a flagless .gnu.version_r;
# or the ten-field row is not a VERNEED, while a flagless VERNEED named .gnu.version_r (the build-id
# note) supplies the heading.
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" shdr-set .gnu.version_r flags 0
check "a flagless .gnu.version_r fails at glibc" "$real" "$dp" "$tmp/needs.so" 1 \
  "glibc: readelf -SW lists 0 ten-field rows named .gnu.version_r, not one"
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" shdr-set .note.gnu.build-id type 0x6ffffffe \
  shdr-set .note.gnu.build-id flags 0 shdr-set .note.gnu.build-id offset "$vr_off" \
  shdr-set .note.gnu.build-id size "$vr_size" shdr-set .note.gnu.build-id link "$str_index" \
  shdr-set .note.gnu.build-id info "$num" shdr-set .gnu.version_r type 1 \
  shdr-set .note.gnu.build-id name "$(sh_name "$drv" .gnu.version_r)"
check "a .gnu.version_r row of type PROGBITS beside a flagless VERNEED of that name fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "glibc: the .gnu.version_r section is of type PROGBITS, not VERNEED"
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" byte $((vr_off + libc + 2)) 0 byte $((vr_off + libc + 3)) 0 \
  replace GLIBC_2.25 GLIBC_2.99
check "a copy whose libc entry counts none of its needs fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "$cn $((total - libc_needs)) $chain"
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" u32 $((vr_off + need + 8)) 0xffff # vna_name
check "a copy with a need whose name readelf cannot read fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "$cn $((total - 1)) $chain"
rm -f "$tmp/needs.so"; patch "$drv" "$tmp/needs.so" dyn-val 0x6fffffff $((num - 1)) shdr-set .gnu.version_r info $((num - 1))
check "a copy whose DT_VERNEEDNUM and .gnu.version_r count one file fewer fails" "$real" "$dp" "$tmp/needs.so" 1 \
  "$cn $((total - $(readelf -VW "$drv" | awk '$4 == "File:" { c = $7 } END { print c }'))) $chain"
# Exercise the fixed resource limits without making the suite build megabyte-scale chains. Each
# failing equal-length source variant lowers one production limit so the real driver's chain reaches
# that branch; removing the production bound makes patch_elf.py or the required diagnostic fail.
limit_case() { # limit_case <id> <old> <new> <expected-exit> <case-name> <message>
  local old="$2" new="$3" want="$4" name="$5" message="$6" d="$tmp/limit-$1" original="$policy_tool" out
  mkdir -p "$d"; cp "$odbc/tools/lib/common.sh" "$d/common.sh"
  out="$(python3 -I "$here/patch_elf.py" "$policy_tool" "$d/elf_policy.sh" replace "$old" "$new" 2>&1)" ||
    { fail "$name (building the lowered-limit policy failed: $out)"; return; }
  policy_tool="$d/elf_policy.sh"
  check "$name" "$real" "$dp" "$drv" "$want" "$message"
  policy_tool="$original"
}
# The size limit admits a section of exactly that size: lowered to the real section's size, the
# driver passes; lowered one byte below it, the driver fails. Each is 8 characters, as 0x100000 is.
printf -v vr_at '0x%06x' "$((vr_size))"; printf -v vr_below '0x%06x' "$((vr_size - 1))"
limit_case size "0x100000" "$vr_at" 0 "the .gnu.version_r size limit admits a section of exactly that size" \
  "fits the Linux policy (newest glibc version required: 2.25)"
limit_case read "0x100000" "$vr_below" 1 "the .gnu.version_r size limit refuses a section one byte over it" \
  "glibc: the .gnu.version_r section exceeds the 1 MiB limit ($((vr_size)) bytes)"
limit_case files "++f > 4096" "++f > 0001" 1 "the 4096-file cap has a failing branch" \
  "glibc: the version-need chain exceeds the 4096-file limit"
limit_case needs "++k > 65536" "++k > 00001" 1 "the 65536-need cap has a failing branch" \
  "glibc: the version-need chain exceeds the 65536-need limit"
# strings: a name in .dynstr with a space, a control character or a literal @ fails before a
# rule parses it. A version name with a newline would end readelf -V's list: here it hides a
# GLIBC_2.99 after it. A literal @ would look like readelf's version suffix on a symbol name.
sc="strings: the driver's .dynstr holds a name with a space, a control character or '@'"
patch "$drv" "$tmp/at.so" replace librsodbc64.so librsodbc64@so
check "a copy with a literal @ in its SONAME fails at strings" "$real" "$dp" "$tmp/at.so" 1 \
  "$sc: librsodbc64@so"
patch "$drv" "$tmp/newline.so" replace GLIBC_2.17 $'GLIBC_2\nXX' replace GLIBC_2.25 GLIBC_2.99
check "a copy with a newline in a version name fails at strings" "$real" "$dp" "$tmp/newline.so" 1 "$sc: GLIBC_2\\012XX"
for c in $'\t:011' $'\x01:001' $'\x7f:177'; do
  rm -f "$tmp/control.so"; patch "$drv" "$tmp/control.so" replace SQLTables "SQL${c%%:*}ables"
  check "a copy with byte ${c#*:} (octal) in an export's name fails at strings" "$real" "$dp" "$tmp/control.so" 1 \
    "$sc: SQL\\${c#*:}ables"
done
# The message names the first such string and ends there, not running on into the strings after it:
# a copy with two bad names, the SONAME and GLIBC_2.25 after it in .dynstr, names only the SONAME.
patch "$drv" "$tmp/two.so" replace librsodbc64.so librsodbc64@so replace GLIBC_2.25 GLIBC@2.25
out="$(bash "$policy_tool" "$real" "$header" "$dp" "$tmp/two.so" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "${out##*$'\n'}" == "$sc: librsodbc64@so" && "$out" != *GLIBC@2.25* ]]; then
  pass "strings names only the first of two bad names"
else fail "strings names only the first of two bad names (exit $status; output: $out)"; fi
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" replace .dynstr .dynstX
check "a copy with no .dynstr section fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: readelf -SW lists 0 ten-field rows named .dynstr, not one"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" replace .dynsym .dynsyX
check "a copy with no .dynsym section fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: readelf -SW lists 0 ten-field rows named .dynsym, not one"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .dynsym flags 0
check "a copy with a flagless .dynsym fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: readelf -SW lists 0 ten-field rows named .dynsym, not one"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .dynstr flags 0
check "a copy with a flagless .dynstr fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: readelf -SW lists 0 ten-field rows named .dynstr, not one"
# Two exact-name ten-field rows: the build-id note renamed .dynsym.
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .note.gnu.build-id name "$(sh_name "$drv" .dynsym)"
check "a copy with two exact-name .dynsym rows fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: readelf -SW lists 2 ten-field rows named .dynsym, not one"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .dynsym type 1
check "a copy whose .dynsym is of type PROGBITS fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: the .dynsym section is of type PROGBITS, not DYNSYM"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .dynstr type 1
check "a copy whose .dynstr is of type PROGBITS fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: the .dynstr section is of type PROGBITS, not STRTAB"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .dynsym link 0
check "a copy whose .dynsym does not link to .dynstr fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: the .dynsym section links to section 0, not to .dynstr (section $str_index)"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-set .gnu.version_r link 0
check "a copy whose .gnu.version_r does not link to .dynstr fails at glibc" "$real" "$dp" "$tmp/view.so" 1 \
  "glibc: the .gnu.version_r section links to section 0, not to .dynstr (section $str_index)"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-add .dynstr size 0x1000000000000000
check "a copy with a 16-digit .dynstr size fails at strings" "$real" "$dp" "$tmp/view.so" 1 "strings: readelf printed '1000000000"
# A section past the end of the file fails naming its rule, not only od's message: .dynstr, and
# .gnu.version_r in a copy whose DT_VERNEED and DT_VERNEEDNUM are retagged, so no count stops it;
# so does a 16-digit .gnu.version_r size.
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" shdr-add .dynstr offset 0x10000000
check "a copy whose .dynstr is past the end of the file fails at strings" "$real" "$dp" "$tmp/view.so" 1 \
  "strings: od cannot read the driver's .dynstr"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" dyn-tag 0x6ffffffe 0x15 dyn-tag 0x6fffffff 0x15 \
  shdr-add .gnu.version_r offset 0x10000000
check "a copy whose .gnu.version_r is past the end of the file fails at glibc" "$real" "$dp" "$tmp/view.so" 1 \
  "glibc: od cannot read the driver's .gnu.version_r"
rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" dyn-tag 0x6ffffffe 0x15 dyn-tag 0x6fffffff 0x15 \
  shdr-add .gnu.version_r size 0x1000000000000000
check "a copy with a 16-digit .gnu.version_r size fails at glibc" "$real" "$dp" "$tmp/view.so" 1 "glibc: readelf printed '1000000000"
# The dynamic-tag and flags rules, on copies of the real driver: its DT_RELACOUNT entry, which
# nothing else here reads, is retagged as each entry that would have the loader bring in another
# object, as an unassigned tag, and as DT_FLAGS_1 with each refused bit.
relacount=0x6ffffff9
for c in "7ffffffd:AUXILIARY:an auxiliary filtee (DT_AUXILIARY)" "7fffffff:FILTER:a filtee (DT_FILTER)" \
  "6ffffefc:AUDIT:an audit library (DT_AUDIT)" "6ffffefb:DEPAUDIT:a dependency audit library (DT_DEPAUDIT)"; do
  IFS=: read -r tag name what <<<"$c"
  patch "$drv" "$tmp/dyn-$name.so" dyn-tag "$relacount" "0x$tag" dyn-val "0x$tag" 0x1
  check "a copy that names $what fails" "$real" "$dp" "$tmp/dyn-$name.so" 1 \
    "dynamic: the driver has dynamic entries that can change how it loads: 0x00000000$tag ($name)"
done
patch "$drv" "$tmp/dyn-unknown.so" dyn-tag "$relacount" 0x30
check "a copy with a dynamic tag the policy does not list fails" "$real" "$dp" "$tmp/dyn-unknown.so" 1 \
  "dynamic: the driver has dynamic entries that can change how it loads: 0x0000000000000030"
for c in "0x2:GLOBAL:in the global symbol scope (DF_1_GLOBAL)" "0x10:LOADFLTR:its filtees loaded at once (DF_1_LOADFLTR)" \
  "0x08000000:PIE:marked a position-independent executable (DF_1_PIE)"; do
  IFS=: read -r bit name what <<<"$c"
  patch "$drv" "$tmp/flags-$name.so" dyn-tag "$relacount" 0x6ffffffb dyn-val 0x6ffffffb "$bit"
  check "a copy $what fails" "$real" "$dp" "$tmp/flags-$name.so" 1 \
    "flags: the driver sets DT_FLAGS_1 bits that change how it loads: $name"
done
# Every bit of DT_FLAGS_1 is read, not only the first: NOW and GLOBAL together must fail at GLOBAL.
patch "$drv" "$tmp/flags-pair.so" dyn-tag "$relacount" 0x6ffffffb dyn-val 0x6ffffffb 0x3
check "a copy with NOW and GLOBAL set fails at GLOBAL" "$real" "$dp" "$tmp/flags-pair.so" 1 \
  "flags: the driver sets DT_FLAGS_1 bits that change how it loads: GLOBAL"
# The tags a release may legitimately gain are on the list, so a driver linked another way still
# passes: each is set here in turn, in place of DT_RELACOUNT.
for tag in 0x4 0x10 0x11 0x12 0x13 0x15 0x18 0x20 0x21 0x22 0x6ffffffa; do
  patch "$drv" "$tmp/dyn-ok.so" dyn-tag "$relacount" "$tag"
  check "a copy whose DT_RELACOUNT is tag $tag passes" "$real" "$dp" "$tmp/dyn-ok.so" 0 "fits the Linux policy"
done
# DT_FLAGS too, with a value the textrel rule accepts (DF_BIND_NOW, not DF_TEXTREL).
patch "$drv" "$tmp/dyn-flags.so" dyn-tag "$relacount" 0x1e dyn-val 0x1e 0x8
check "a copy with DT_FLAGS BIND_NOW passes" "$real" "$dp" "$tmp/dyn-flags.so" 0 "fits the Linux policy"
# A processor-specific tag, which a branch-protection or CET build writes about its own code,
# passes; the three at the top of that range that name another object do not.
patch "$drv" "$tmp/dyn-proc.so" dyn-tag "$relacount" 0x70000001
check "a copy with a processor-specific tag passes" "$real" "$dp" "$tmp/dyn-proc.so" 0 "fits the Linux policy"
patch "$drv" "$tmp/dyn-used.so" dyn-tag "$relacount" 0x7ffffffe dyn-val 0x7ffffffe 0x1
check "a copy with DT_USED fails" "$real" "$dp" "$tmp/dyn-used.so" 1 "dynamic: the driver has dynamic entries"
# The tags that are not on the list fail, whichever they are, not only the four the rule names.
for tag in 0x6ffffefa 0x6ffffdfd 0x26; do
  patch "$drv" "$tmp/dyn-bad.so" dyn-tag "$relacount" "$tag"
  check "a copy with the unlisted tag $tag fails" "$real" "$dp" "$tmp/dyn-bad.so" 1 \
    "dynamic: the driver has dynamic entries that can change how it loads"
done
# dynamic-view: the rules read the .dynamic section, the loader the table at PT_DYNAMIC's address,
# so a copy where the two differ fails there, before any rule that reads readelf -d.
view() { # view <what> <message> <change>...
  local what="$1" want="$2"; shift 2
  rm -f "$tmp/view.so"; patch "$drv" "$tmp/view.so" "$@"
  check "a copy with $what fails at dynamic-view" "$real" "$dp" "$tmp/view.so" 1 "dynamic-view: $want"
}
view "a PT_DYNAMIC one entry shorter than .dynamic" "PT_DYNAMIC (offset" phdr-add 2 filesz -16
view "PT_DYNAMIC one entry later in the file" "PT_DYNAMIC (offset" phdr-add 2 offset 16
# The PT_LOAD must map PT_DYNAMIC's address to its offset: one entry later in memory, it maps that
# address to the next entry in the file, not to the offset readelf reads.
view "PT_DYNAMIC one entry later in memory" "no PT_LOAD maps PT_DYNAMIC's address" phdr-add 2 vaddr 16
# The PT_LOAD must hold the whole table in its file bytes. The table is moved consistently, with
# .dynamic: below the writable PT_LOAD, past its file bytes (into the zero-filled part the loader
# does not read from the file), and to start 0x100 bytes before their end, so that only the table's
# end is past them.
read -r t_off t_addr t_size <<<"$(readelf -lW "$drv" | awk '$1 == "DYNAMIC" { print $2, $3, $5 }')"
straddle="$(readelf -lW "$drv" | awk '$1 == "LOAD" { print $2, $5 }' | while read -r l_off l_size; do
  if ((l_off <= t_off && t_off < l_off + l_size)); then printf '0x%x\n' $((l_off + l_size - 0x100 - t_off)); fi; done)"
for d in -0x60000 0x30000 "$straddle"; do
  view "the table moved by $d, outside its PT_LOAD's file bytes" "no PT_LOAD maps PT_DYNAMIC's address" \
    phdr-add 2 offset "$d" phdr-add 2 vaddr "$d" shdr-add .dynamic offset "$d" shdr-add .dynamic addr "$d"
done
# Each PT_LOAD must start at or above the end of the last page of the one before it. The loader maps
# each PT_LOAD in turn over the pages from its address to its end (its file bytes, then zeros to
# memsz), so a later one on another's pages decides what the loader reads there. Each copy retypes
# the PT_NOTE, the header after both PT_LOADs, as such a PT_LOAD: over the table from other file
# bytes, as zeros only (filesz 0), as file bytes only (memsz 0, which the loader maps all the same),
# one byte at the start of the table's page, 16 bytes just before and just after the table, on a
# page of its own (the table straddles two 4 KiB pages), and over the page of .dynsym or
# .gnu.version, which the loader reads for dlsym while readelf reads the sections' own bytes. Each
# is a shape the loader accepts: the align and the offset keep the address congruent to the offset.
policy_page=0x1000; [[ "$dp" == linux_amd64 ]] || policy_page=0x10000
below="where the pages of the PT_LOAD before it end"
make_overlay() { # make_overlay <driver> <vaddr> <filesz> <memsz> <alignment>
  local in="$1" vaddr="$2" filesz="$3" memsz="$4" alignment="$5"
  rm -f "$tmp/view.so"; patch "$in" "$tmp/view.so" phdr-set 4 align "$alignment" \
    phdr-set 4 offset $((vaddr % alignment)) phdr-set 4 vaddr "$vaddr" \
    phdr-set 4 filesz "$filesz" phdr-set 4 memsz "$memsz" phdr-type 4 1
}
overlay() { # overlay <fails|passes> <what> <vaddr> <filesz> <memsz>, on the default driver
  make_overlay "$drv" "$3" "$4" "$5" "$policy_page"
  if [[ "$1" == passes ]]; then check "a copy with a later PT_LOAD $2 passes" "$real" "$dp" "$tmp/view.so" 0 "fits"
  else check "a copy with a later PT_LOAD $2 fails at dynamic-view" "$real" "$dp" "$tmp/view.so" 1 "$below"; fi
}
overlay fails "over the table, from other file bytes" "$t_addr" "$t_size" "$t_size"
t_page=$((t_addr & -policy_page)) # from the table's page start, so only the size decides
overlay fails "of zeros over the table" "$t_page" 0 $((t_addr + t_size - t_page))
overlay fails "over the table with a memsz of 0" "$t_page" $((t_addr + t_size - t_page)) 0
overlay fails "of one byte at the start of the table's page" "$t_page" 1 1
overlay fails "on a page of the table, before it" $((t_addr - 32)) 16 16
overlay fails "on a page of the table, after it" $((t_addr + t_size + 16)) 16 16
for s in .dynsym .gnu.version; do
  a="$(readelf -SW "$drv" | awk -v s="$s" '{ sub(/^ *\[ *[0-9]+\] +/, "") } $1 == s { print $3 }')"
  overlay fails "over the page of $s" $((0x$a)) 16 16
done
# The rule's page arithmetic, against the PT_LOADs around it: the end of each PT_LOAD's memory or
# file bytes, whichever is larger, rounded up to a page, is where the next may start. A PT_LOAD on
# the page after the last one's end passes; 16 bytes after that end, on its last page, fail; one on
# pages of its own between the two fails, since the PT_LOADs must be in address order; and so does
# the first PT_LOAD with its zeros, or only its file bytes, reaching one byte onto the next one's
# first page.
read -r f_addr f_file f_mem l_addr l_file l_mem <<<"$(readelf -lW "$drv" | awk '$1 == "LOAD" { print $3, $5, $6 }' | sed -n '1p;$p' | tr '\n' ' ')"
f_end=$((f_addr + (f_mem > f_file ? f_mem : f_file))) l_end=$((l_addr + (l_mem > l_file ? l_mem : l_file)))
overlay passes "on the page after the last PT_LOAD's end" \
  $(((l_end + policy_page - 1) & -policy_page)) 16 16
# Just after the last PT_LOAD's end, still on its last page: 16 bytes after it where the page has
# room, else half the room left (2.3.1.0's x86_64 library ends 8 bytes before a page boundary).
l_room=$((((l_end + policy_page - 1) & -policy_page) - l_end))
if ((l_room > 1)); then
  overlay fails "just after the last PT_LOAD's end, on its last page" $((l_end + (l_room > 32 ? 16 : l_room / 2))) 16 16
else n=$((n+1)); echo "ok $n - a later PT_LOAD on the last PT_LOAD's last page # SKIP the last PT_LOAD ends at a page boundary"; fi
overlay fails "between the two, on pages of its own" $(((f_end + policy_page - 1) & -policy_page)) 16 16
next_page=$((l_addr & -policy_page))
next="$(printf 'the PT_LOAD at 0x%016x starts below 0x%x' "$l_addr" $((next_page + policy_page)))"
view "the first PT_LOAD's zeros reaching onto the next one's first page" "$next" phdr-set 1 memsz $((next_page + 1 - f_addr))
view "the first PT_LOAD's file bytes, past its memsz, reaching onto the next one's first page" "$next" \
  phdr-set 1 filesz $((next_page + 1 - f_addr))
# The page is the platform's: 4 KiB on x86-64, and up to 64 KiB on aarch64. 16 bytes at the end of
# the last PT_LOAD's last 64 KiB page are on another 4 KiB page than its end, so such a copy passes
# for amd64 and fails for arm64. The fixture uses 64 KiB alignment, valid on either architecture.
for p in linux_amd64 linux_arm64; do
  [[ -f "$tmp/$p.so" ]] || { echo "ok - $p page size # SKIP the lock under test has no $p row"; continue; }
  read -r a f m <<<"$(readelf -lW "$tmp/$p.so" | awk '$1 == "LOAD" { print $3, $5, $6 }' | tail -1)"
  at=$((((a + (m > f ? m : f) + 0x10000 - 1) & -0x10000) - 16))
  name="a copy with a later PT_LOAD at the end of the last PT_LOAD's 64 KiB page, for $p"
  make_overlay "$tmp/$p.so" "$at" 16 16 0x10000
  if [[ "$p" == linux_amd64 ]]; then check "$name passes" "$real" "$p" "$tmp/view.so" 0 "fits"
  else check "$name fails at dynamic-view" "$real" "$p" "$tmp/view.so" 1 "$below"; fi
done
# No PT_DYNAMIC or no .dynamic fails, and so does a value of 16 hex digits or more, which the rule
# refuses so that its sums cannot overflow.
view "no PT_DYNAMIC" "the driver needs exactly one PT_DYNAMIC and one .dynamic section" phdr-type 2 0
view "no .dynamic section" "the driver needs exactly one PT_DYNAMIC and one .dynamic section" replace .dynamic .dynamiX
view "a 16-digit .dynamic size" "readelf printed '1000000000000" shdr-add .dynamic size 0x1000000000000000
view "a 16-digit PT_DYNAMIC size" "readelf printed '0x1000000000000" phdr-add 2 filesz 0x1000000000000000
view "a 16-digit PT_LOAD address" "readelf printed '0x1000000000000" phdr-add 1 vaddr 0x1000000000000000
view "a 16-digit PT_LOAD memsz" "readelf printed '0x1000000000000" phdr-set 1 memsz 0x1000000000000000
view "a 16-digit PT_LOAD filesz" "readelf printed '0x1000000000000" phdr-set 1 filesz 0x1000000000000000
view "a 16-digit PT_LOAD offset" "readelf printed '0x1000000000000" phdr-set 1 offset 0x1000000000000000
view "a second PT_DYNAMIC" "the driver needs exactly one PT_DYNAMIC and one .dynamic section" phdr-type 4 2
view "a second .dynamic section" "the driver needs exactly one PT_DYNAMIC and one .dynamic section" replace .comment .dynamic
# A .dynamic of type NOBITS gives readelf -d no table ("There is no dynamic section in this file"),
# so a DT_RUNPATH or a DT_AUDIT in the loader's table would pass every rule that reads readelf -d.
dyn_type=$((shoff + 64 * $(readelf -SW "$drv" | awk '{ sub(/^ *\[ */, "") } $2 == ".dynamic" { print $1 + 0 }') + 4))
view "a NOBITS .dynamic and its SONAME retagged DT_RUNPATH" "the .dynamic section is of type NOBITS, not DYNAMIC" \
  byte "$dyn_type" 8 dyn-tag 0xe 0x1d
view "a NOBITS .dynamic and a DT_AUDIT" "the .dynamic section is of type NOBITS, not DYNAMIC" \
  byte "$dyn_type" 8 dyn-tag "$relacount" 0x6ffffefc dyn-val 0x6ffffefc 1
# The type must be DYNAMIC, not merely other than NOBITS.
view "a PROGBITS .dynamic" "the .dynamic section is of type PROGBITS, not DYNAMIC" byte "$dyn_type" 1
# readelf -d must say it reads the table at that offset, which it does not for a table of size 0.
view "a PT_DYNAMIC and a .dynamic of size 0" "readelf -d does not read the table at the .dynamic section's offset" \
  phdr-set 2 filesz 0 phdr-set 2 memsz 0 shdr-add .dynamic size "-$t_size"
# With no DT_NULL in the table, readelf stops at the section's end and the loader reads on.
view "no DT_NULL in .dynamic" "the .dynamic section has no DT_NULL" dyn-tag-all 0 0x15
# Every byte of each d_tag is read, not only the first ones: this tag's only nonzero byte is its
# last, byte 7.
view "every DT_NULL retagged 0x0100000000000000" "the .dynamic section has no DT_NULL" \
  dyn-tag-all 0 0x0100000000000000
# The DT_NULL is read from the table's bytes: a SONAME that prints a NULL row of its own in readelf
# -d's text does not stand in for it.
view "no DT_NULL and a SONAME that prints a NULL row" "the .dynamic section has no DT_NULL" dyn-tag-all 0 0x15 \
  replace librsodbc64.so $'lib\n0x0 (NULL)'
# This fixture gives its A-flagged row more than ten fields, so it does not match .dynamic.
# Other crafted name and flag combinations can shift fields; those are an accepted residual.
view "a section name that reads as the .dynamic row" "the driver needs exactly one PT_DYNAMIC and one .dynamic section" \
  replace .dynamic .dynamiX replace .gnu.version_r ".dynamic 0 0 x"
# patch_elf.py fails, naming what it did not find, rather than write a copy without the change.
for c in "shdr-add .nosuch offset 16:no section .nosuch" "dyn-tag-all 0x7fff 0:nothing with 32767" \
  "dynsym SQLNoSuch value 0:no symbol SQLNoSuch"; do
  read -r -a change <<<"${c%%:*}"
  rm -f "$tmp/unchanged.so"
  out="$(python3 -I "$here/patch_elf.py" "$drv" "$tmp/unchanged.so" "${change[@]}" 2>&1)" && status=0 || status=$?
  if [[ "$status" == 1 && "$out" == "${c#*:}" && ! -e "$tmp/unchanged.so" ]]; then
    pass "patch_elf.py's ${change[0]} fails on ${change[1]}, which the driver lacks"; else fail "patch_elf.py ${change[*]} (exit $status; $out)"; fi
done
patch "$drv" "$tmp/no-stack.so" phdr-type 0x6474e551 0
check "a copy with no PT_GNU_STACK fails" "$real" "$dp" "$tmp/no-stack.so" 1 "stack: the driver has no single PT_GNU_STACK"
patch "$drv" "$tmp/two-stacks.so" phdr-type 4 0x6474e551
check "a copy with two PT_GNU_STACK fails" "$real" "$dp" "$tmp/two-stacks.so" 1 "stack: the driver has no single PT_GNU_STACK"
# The floor is compared number by number (2.3.4 is older than 2.25), and must be one version.
floor() { local d="$tmp/floor-$1"; mkdir -p "$d"; cp "$real"/*.txt "$d/"; printf "$2" > "$d/glibc-floor.txt"; echo "$d"; }
check "a floor of 2.25 admits the driver's GLIBC_2.25" "$(floor 225 '2.25\n')" "$dp" "$drv" 0 "fits"
# Every component is read as a decimal number, leading zero and all: a floor of 2.030 is 2.30, so
# it admits GLIBC_2.25, where reading 030 as octal (24) would refuse it.
check "a floor of 2.030 admits GLIBC_2.25, 030 read as 30" "$(floor 2030 '2.030\n')" "$dp" "$drv" 0 "fits"
# The other direction, on the copy whose 2.25 became 2.08: with a floor of 2.08, which is 2.8, its
# remaining needs are all newer, so it is refused. A comparison that fails on the leading zero
# would accept it.
check "a floor of 2.08 refuses the copy whose newest need is 2.18" "$(floor 208 '2.08\n')" "$dp" "$tmp/zero.so" 1 \
  "glibc: the driver requires GLIBC_2.10"
check "a floor of 2.24 refuses it, naming each version above" "$(floor 224 '2.24\n')" "$dp" "$drv" 1 "glibc: the driver requires GLIBC_2.25, newer than the floor 2.24"
# readelf translates its headings, which the rules parse, so they run it in the C locale: a caller with a French
# LANG, and no LC_ALL, cannot hide a need from the glibc rule. The stand-in translates only that rule's heading
# (the real readelf otherwise in English) unless LC_ALL=C reaches it, as only an exported one does.
loc="$tmp/translating"; mkdir -p "$loc"
printf '#!/bin/sh\nif [ "${LC_ALL:-}" = C ]; then exec %s "$@"; fi\nLC_ALL=C %s "$@" | sed "s/^Version needs section/Version nécessitant la section/"\n' \
  "$(command -v readelf)" "$(command -v readelf)" > "$loc/readelf"
chmod +x "$loc/readelf"
out="$(env -u LC_ALL PATH="$loc:$PATH" LANG=fr_FR.UTF-8 bash "$policy_tool" "$(floor 224-fr '2.24\n')" "$header" "$dp" "$drv" 2>&1)" &&
  status=0 || status=$?
if [[ "$status" == 1 && "$out" == *"glibc: the driver requires GLIBC_2.25, newer than the floor 2.24"* ]]; then
  pass "a caller's locale does not hide a glibc need from the rules"; else fail "a translated readelf (exit $status; $out)"; fi
check "a floor of 2.16 names every version above it" "$(floor 216 '2.16\n')" "$dp" "$drv" 1 "requires GLIBC_2.17 GLIBC_2.18 GLIBC_2.25, newer than the floor 2.16"
for bad in "abc" "2.28\n2.30" "" "3"; do
  check "a floor of '${bad//\\n/ and }' is not a version" "$(floor "x${#bad}" "$bad\n")" "$dp" "$drv" 1 "policy: glibc-floor.txt must hold one version"
done
# A readelf that fails on one of the rules' reads fails the run there, naming the read, even after
# printing the whole listing: the stand-in runs the real readelf and then exits 1.
broken="$tmp/failing-readelf"; mkdir -p "$broken"
for c in "-lW:dynamic-view: readelf cannot read the driver's program headers" \
  "-SW:dynamic-view: readelf cannot read the driver's sections" \
  "-dW:dynamic-view: readelf cannot read the driver's dynamic section" \
  "--dyn-syms:exports: readelf cannot read the driver's dynamic symbols" \
  "-tW:strings: readelf cannot read the driver's section names" \
  "-VW:exports: readelf cannot read the driver's version sections"; do
  printf '#!/bin/sh\nif [ "$1" = "%s" ]; then %s "$@"; echo "readelf: Error: stand-in failure" >&2; exit 1; fi\nexec %s "$@"\n' \
    "${c%%:*}" "$(command -v readelf)" "$(command -v readelf)" > "$broken/readelf"
  chmod +x "$broken/readelf"
  PATH="$broken:$PATH" check "a readelf that fails on ${c%%:*} fails the run" "$real" "$dp" "$drv" 1 "${c#*:}"
done
# readelf -d must say it reads the table at .dynamic's offset: a stand-in that names another fails.
printf '#!/bin/sh\nif [ "$1" = -dW ]; then %s "$@" | sed "s/^Dynamic section at offset 0x[0-9a-f]*/&0/"; exit 0; fi\nexec %s "$@"\n' \
  "$(command -v readelf)" "$(command -v readelf)" > "$broken/readelf"
PATH="$broken:$PATH" check "a readelf -d that reads the table at another offset fails the run" "$real" "$dp" "$drv" 1 \
  "dynamic-view: readelf -d does not read the table at the .dynamic section's offset"

if [[ -z "$host" ]] || ! command -v gcc >/dev/null; then
  echo "ok - the cases on libraries built here # SKIP no Linux compiler for $(uname -s) $(uname -m)"
  echo "# $n cases, $((n - failed)) passed"
  exit $(( failed > 0 ))
fi
# Small libraries. The test policy: the interface's entry points and one more export, libc only.
# The entry points come from the compiler's preprocessor expanding the header's macro, as the
# extension's own build does, not from a copy of the rule's parser, so a change to that parser that
# reads a different list fails the cases below.
pol="$tmp/policy"; mkdir -p "$pol"
printf '#include "odbc_driver.hpp"\n#define ENTRY(name) listed_entry_point SQL##name\nREDSHIFT_ODBC_ENTRY_POINTS(ENTRY)\n' > "$tmp/entries.cpp"
if out="$(gcc -E -P -x c++ -I "$repo/src/transport" -I "$repo/third_party/unixodbc" -o "$tmp/entries.i" "$tmp/entries.cpp" 2>&1)"; then
  grep -o 'listed_entry_point SQL[A-Za-z0-9_]*' "$tmp/entries.i" | sed 's/^listed_entry_point //' | LC_ALL=C sort -u > "$tmp/entries"
else
  # Every library case below needs the list: one named failure, rather than one per case.
  fail "preprocessing the interface header failed (the suite needs gcc's C++ front end, g++): $out"
  echo "# $n cases, $((n - failed)) passed"
  exit 1
fi
{ cat "$tmp/entries"; echo SQLBrowseConnect; } > "$pol/expected-exports.txt"
echo libc.so.6 > "$pol/allowed-needed.txt"; echo 2.28 > "$pol/glibc-floor.txt"
printf '{ global: SQL*; local: *; };\n' > "$tmp/exports.map"
# lib <name> <exports-file> [<flag>...]: a shared library defining each name in <exports-file>,
# exported by the version script $LIB_MAP (by default every SQL* name, unversioned). A build
# failure is a failed case, naming the compiler's message; the cases that use it then fail.
# Its segments are 64 KiB apart, the largest page dynamic-view rounds to, whatever the linker's
# default: on an aarch64 host, where dynamic-view rounds to 64 KiB, segments 4 KiB apart would start
# below the previous PT_LOAD's 64 KiB-rounded end, and every case below would fail there instead of
# at its rule.
lib() {
  local name="$1" names="$2" out; shift 2
  awk '{ printf "int %s(void) { return 0; }\n", $1 }' "$names" > "$tmp/$name.c"
  out="$(gcc -shared -fPIC -O0 -Wl,-z,max-page-size=0x10000 -Wl,--version-script="${LIB_MAP:-$tmp/exports.map}" -o "$tmp/$name.so" "$tmp/$name.c" "$@" 2>&1)" ||
    { fail "building the $name test library failed: $out"; rm -f "$tmp/$name.so"; }
}
# Linked to libc whatever the compiler's default (Ubuntu links --as-needed, which would leave base.so no DT_NEEDED for
# the DT_NEEDED cases below to patch).
lib base "$pol/expected-exports.txt" -Wl,--no-as-needed -lc
check "a library with exactly the policy's exports passes" "$pol" "$host" "$tmp/base.so" 0 "fits the Linux policy"
{ cat "$pol/expected-exports.txt"; echo SQLExtra; } > "$tmp/extra.txt"; lib extra "$tmp/extra.txt"
check "an extra export fails" "$pol" "$host" "$tmp/extra.so" 1 "exports: the driver's exports differ from expected-exports.txt; extra: SQLExtra; missing: "
# An extra export renamed, at the same length, to "UND evil" would read as the undefined name
# "evil" and so as no export at all; strings refuses the space first.
[[ ! -f "$tmp/extra.so" ]] || patch "$tmp/extra.so" "$tmp/und.so" replace SQLExtra "UND evil"
check "an extra export named 'UND evil' fails at strings" "$pol" "$host" "$tmp/und.so" 1 "$sc: UND\\040evil"
# This A-flagged fake row has too many fields for the helper, so this fixture fails before the
# renamed table is scanned. Other crafted rows can shift into the selected form under the accepted
# residual.
[[ ! -f "$tmp/und.so" ]] || patch "$tmp/und.so" "$tmp/fake-dynstr.so" \
  replace .dynstr .dynstX replace .note.gnu.build-id ".dynstr a b 0 0 zz"
check "an A-flagged .dynstr-like row outside the selected form fails" "$pol" "$host" \
  "$tmp/fake-dynstr.so" 1 "strings: readelf -SW lists 0 ten-field rows named .dynstr, not one"
# A weak definition, and an absolute symbol that names no version definition, are exports too.
printf '__attribute__((weak)) int SQLExtra(void) { return 0; }\n' > "$tmp/weak-extra.c"
lib weak "$pol/expected-exports.txt" "$tmp/weak-extra.c"
check "an extra weak export fails" "$pol" "$host" "$tmp/weak.so" 1 "exports: the driver's exports differ from expected-exports.txt; extra: SQLExtra; missing: "
lib absolute "$pol/expected-exports.txt" -Wl,--defsym=SQLExtra=0x10
check "an extra absolute export fails" "$pol" "$host" "$tmp/absolute.so" 1 "exports: the driver's exports differ from expected-exports.txt; extra: SQLExtra; missing: "
lib missing "$tmp/entries"
check "a missing export fails" "$pol" "$host" "$tmp/missing.so" 1 "exports: the driver's exports differ from expected-exports.txt; extra: ; missing: SQLBrowseConnect"
# The rule reads exactly the compiler's list: a library and policy with none of the entry points
# fail, naming every one, in order, and nothing else.
pol3="$tmp/policy-no-entries"; mkdir -p "$pol3"; cp "$pol"/*.txt "$pol3/"; echo SQLBrowseConnect > "$pol3/expected-exports.txt"
lib no-entries "$pol3/expected-exports.txt"
out="$(bash "$policy_tool" "$pol3" "$header" "$host" "$tmp/no-entries.so" 2>&1)" && status=0 || status=$?
want="entry-points: the interface's entry points are not all in expected-exports.txt; missing: $(tr '\n' ' ' < "$tmp/entries" | sed 's/ $//')"
if [[ "$status" == 1 && -s "$tmp/entries" && "$out" == "$want" ]]; then
  pass "the rule reads the entry points the compiler reads, $(wc -l < "$tmp/entries" | tr -d ' ') of them"
else fail "the entry points read (exit $status; got: $out; wanted: $want)"; fi
pol2="$tmp/policy-no-entry"; mkdir -p "$pol2"; cp "$pol"/*.txt "$pol2/"
grep -v '^SQLGetDiagField$' "$pol/expected-exports.txt" > "$pol2/expected-exports.txt"; lib no-entry "$pol2/expected-exports.txt"
check "an interface entry point missing from the list fails" "$pol2" "$host" "$tmp/no-entry.so" 1 "entry-points: the interface's entry points are not all in expected-exports.txt; missing: SQLGetDiagField"
sed 's/REDSHIFT_ODBC_ENTRY_POINTS(X)/REDSHIFT_ODBC_ENTRIES(X)/' "$header" > "$tmp/renamed.hpp"
check "a header without the REDSHIFT_ODBC_ENTRY_POINTS(X) list fails" "$pol" "$host" "$tmp/base.so" 1 "has no REDSHIFT_ODBC_ENTRY_POINTS(X) list" "$tmp/renamed.hpp"
check "a missing interface header fails at the entry points" "$pol" "$host" "$tmp/base.so" 1 "entry-points: the interface header $tmp/none.hpp is missing" "$tmp/none.hpp"
lib needed "$pol/expected-exports.txt" -Wl,--no-as-needed -lm
check "an extra DT_NEEDED fails" "$pol" "$host" "$tmp/needed.so" 1 "needed: the driver needs libraries not in allowed-needed.txt: libm.so.6"
[[ ! -f "$tmp/base.so" ]] || patch "$tmp/base.so" "$tmp/unnamed.so" dyn-val 1 0xfffff0
check "a DT_NEEDED whose name readelf cannot read fails" "$pol" "$host" "$tmp/unnamed.so" 1 "needed: the driver has a DT_NEEDED entry whose name readelf cannot read"
lib rpath "$pol/expected-exports.txt" -Wl,--disable-new-dtags,-rpath,/opt/x
check "an RPATH fails" "$pol" "$host" "$tmp/rpath.so" 1 "search-path: the driver has a library search path: (RPATH) Library rpath: [/opt/x]"
lib runpath "$pol/expected-exports.txt" -Wl,--enable-new-dtags,-rpath,/opt/x
check "a RUNPATH fails" "$pol" "$host" "$tmp/runpath.so" 1 "search-path: the driver has a library search path: (RUNPATH) Library runpath: [/opt/x]"
printf '__asm__(".text\\n.quad SQLAllocHandle\\n");\n' > "$tmp/textrel-extra.c"
lib textrel "$pol/expected-exports.txt" "$tmp/textrel-extra.c" -Wl,-z,notext -Wl,--enable-new-dtags
check "a text relocation fails" "$pol" "$host" "$tmp/textrel.so" 1 "textrel: the driver has text relocations"
if [[ -f "$tmp/textrel.so" ]]; then
  patch "$tmp/textrel.so" "$tmp/textrel-flags.so" dyn-tag 0x16 0x15   # DT_TEXTREL becomes DT_DEBUG
  patch "$tmp/textrel.so" "$tmp/textrel-tag.so" dyn-clear 0x1e 0x4    # DF_TEXTREL cleared in DT_FLAGS
fi
check "DF_TEXTREL alone fails" "$pol" "$host" "$tmp/textrel-flags.so" 1 "textrel: the driver has text relocations"
check "DT_TEXTREL alone fails" "$pol" "$host" "$tmp/textrel-tag.so" 1 "textrel: the driver has text relocations"
# The dynamic entries a linker writes for a filtee or an audit library, which the loader would
# bring in: one library per rule, built here.
for c in "-f:AUXILIARY:7ffffffd:-Wl,-f,libaux.so" "-F:FILTER:7fffffff:-Wl,-F,libflt.so" \
  "audit:AUDIT:6ffffefc:-Wl,--audit=libaud.so" "depaudit:DEPAUDIT:6ffffefb:-Wl,--depaudit=libdep.so"; do
  IFS=: read -r option name tag flag <<<"$c"
  lib "dyn$name" "$pol/expected-exports.txt" "$flag"
  if [[ -f "$tmp/dyn$name.so" ]] && ! readelf -dW "$tmp/dyn$name.so" | grep -q "($name)"; then
    n=$((n+1)); echo "ok $n - a library linked with $option fails # SKIP this linker writes no DT_$name"
  else
    check "a library linked with $option fails at the dynamic rule" "$pol" "$host" "$tmp/dyn$name.so" 1 \
      "dynamic: the driver has dynamic entries that can change how it loads: 0x00000000$tag ($name)"
  fi
done
lib global "$pol/expected-exports.txt" -Wl,-z,global
check "a library linked into the global scope fails at the flags rule" "$pol" "$host" "$tmp/global.so" 1 \
  "flags: the driver sets DT_FLAGS_1 bits that change how it loads: GLOBAL"
# The exports are compared without their version suffix: a library whose symbols are all version
# V1 passes a policy listing the plain names. Without the suffix stripped, every name would differ;
# without the version's own absolute symbol (V1) set aside, it would be an extra export.
printf 'V1 { global: SQL*; local: *; };\n' > "$tmp/versioned.map"
printf 'int SQLBrowseConnect(void) { return 0; }\n' > "$tmp/browse.c"
LIB_MAP="$tmp/versioned.map" lib versioned "$tmp/entries" "$tmp/browse.c"
if [[ -f "$tmp/versioned.so" ]] && ! readelf --dyn-syms -W "$tmp/versioned.so" | grep -q ' SQLBrowseConnect@@V1$'; then
  n=$((n+1)); echo "ok $n - a library whose exports carry a version passes # SKIP this readelf prints no @@version suffix"
else
  check "a library whose exports carry a version passes, the suffix stripped" "$pol" "$host" "$tmp/versioned.so" 0 "fits the Linux policy"
fi
[[ ! -f "$tmp/versioned.so" ]] || patch "$tmp/versioned.so" "$tmp/version-def-link.so" \
  shdr-set .gnu.version_d link 0
check "a .gnu.version_d section not linked to .dynstr fails" "$pol" "$host" "$tmp/version-def-link.so" 1 \
  "exports: the .gnu.version_d section links to section 0, not to .dynstr"
# readelf -V finds VERDEF sections by type even when their Flg column is empty and section() drops
# their nine-field -SW rows. A flagless second header over the same bytes must still be counted.
if [[ -f "$tmp/versioned.so" ]]; then
  read -r vd_off vd_size vd_link vd_info <<<"$(readelf -SW "$tmp/versioned.so" | awk '
    { sub(/^ *\[ *[0-9]+\] +/, "") }
    $1 == ".gnu.version_d" { print "0x" $4, "0x" $5, $8, $9 }')"
  patch "$tmp/versioned.so" "$tmp/version-def-flagless.so" \
    shdr-set .note.gnu.build-id type 0x6ffffffd shdr-set .note.gnu.build-id flags 0 \
    shdr-set .note.gnu.build-id offset "$vd_off" shdr-set .note.gnu.build-id size "$vd_size" \
    shdr-set .note.gnu.build-id link "$vd_link" shdr-set .note.gnu.build-id info "$vd_info"
fi
check "a flagless second VERDEF section fails" "$pol" "$host" "$tmp/version-def-flagless.so" 1 \
  "exports: readelf -V must list at most one version-definition section, named .gnu.version_d"
# The same copy with that second header also named .gnu.version_d: both headings have the exact
# name, so only the at-most-one count refuses it. And the one exact-name heading without a matching
# row: the real .gnu.version_d flagless; or its ten-field row of type PROGBITS while the flagless
# copy, named .gnu.version_d, supplies the heading.
if [[ -f "$tmp/version-def-flagless.so" ]]; then
  vd_name="$(sh_name "$tmp/versioned.so" .gnu.version_d)"
  patch "$tmp/version-def-flagless.so" "$tmp/version-def-two.so" shdr-set .note.gnu.build-id name "$vd_name"
  patch "$tmp/versioned.so" "$tmp/version-def-flags.so" shdr-set .gnu.version_d flags 0
  patch "$tmp/version-def-flagless.so" "$tmp/version-def-type.so" shdr-set .gnu.version_d type 1 \
    shdr-set .note.gnu.build-id name "$vd_name"
fi
check "a flagless second VERDEF section also named .gnu.version_d fails" "$pol" "$host" "$tmp/version-def-two.so" 1 \
  "exports: readelf -V must list at most one version-definition section, named .gnu.version_d"
check "a flagless .gnu.version_d fails at exports" "$pol" "$host" "$tmp/version-def-flags.so" 1 \
  "exports: readelf -SW lists 0 ten-field rows named .gnu.version_d, not one"
check "a .gnu.version_d row of type PROGBITS beside a flagless VERDEF of that name fails" "$pol" "$host" \
  "$tmp/version-def-type.so" 1 "exports: the .gnu.version_d section is of type PROGBITS, not VERDEF"
# A space changes the VERDEF heading's exact name while readelf -V still lists its definitions.
# The heading-name check must fail before those names can set aside absolute symbols.
[[ ! -f "$tmp/versioned.so" ]] || patch "$tmp/versioned.so" "$tmp/version-def-row.so" \
  replace .gnu.version_d ".gnu.version d"
check "version definitions with no matching exact heading name fail" "$pol" "$host" \
  "$tmp/version-def-row.so" 1 \
  "exports: readelf -V must list at most one version-definition section, named .gnu.version_d"
# Each expected export must be defined once in the default version, as a non-absolute function, the
# one dlsym returns: not a data object or a thread-local, not a function only under a hidden
# version, and not defined in a second default version as well (dlsym then finds none). A hidden
# version beside the default one is fine.
nf="exports: expected exports that are not one non-absolute function of the default version"
printf 'int SQLBrowseConnect = 1;\n' > "$tmp/data-object.c"
lib data "$tmp/entries" "$tmp/data-object.c"
check "an expected export that is a data object fails" "$pol" "$host" "$tmp/data.so" 1 "$nf: SQLBrowseConnect (OBJECT)"
printf '__thread int SQLBrowseConnect = 1;\n' > "$tmp/thread-local.c"
lib tls "$tmp/entries" "$tmp/thread-local.c"
check "an expected export that is a thread-local fails" "$pol" "$host" "$tmp/tls.so" 1 "$nf: SQLBrowseConnect (TLS)"
printf 'V0 { global: SQLBrowseConnect; local: *; };\nV1 { global: SQL*; local: *; } V0;\n' > "$tmp/hidden.map"
printf 'int SQLBrowseConnect(void) { return 0; }\n__asm__(".symver SQLBrowseConnect, SQLBrowseConnect@V0");\n' > "$tmp/hidden-only.c"
LIB_MAP="$tmp/hidden.map" lib hidden "$tmp/entries" "$tmp/hidden-only.c"
check "an expected export only under a hidden version fails" "$pol" "$host" "$tmp/hidden.so" 1 "$nf: SQLBrowseConnect@V0 (FUNC)"
printf 'int browse_old(void) { return 0; }\nint browse_new(void) { return 1; }\n__asm__(".symver browse_old, SQLBrowseConnect@V0");\n__asm__(".symver browse_new, SQLBrowseConnect@@V1");\n' > "$tmp/both.c"
LIB_MAP="$tmp/hidden.map" lib both-versions "$tmp/entries" "$tmp/both.c"
check "an expected export with a hidden and a default version passes" "$pol" "$host" "$tmp/both-versions.so" 0 "fits the Linux policy"
# Two default versions of one name, the second a function or a data object: GNU ld links either.
printf 'V0 { global: SQL*; local: *; };\nV1 { global: SQL*; } V0;\n' > "$tmp/two.map"
for second in "int second_definition(void) { return 1; }:function" "int second_definition = 1;:data object"; do
  printf '%s\n__asm__(".symver second_definition, SQLBrowseConnect@@V0");\n' "${second%:*}" > "$tmp/second.c"
  rm -f "$tmp/two-defaults.so"; LIB_MAP="$tmp/two.map" lib two-defaults "$pol/expected-exports.txt" "$tmp/second.c"
  check "an expected export also defined in a second default version as a ${second##*:} fails" "$pol" "$host" \
    "$tmp/two-defaults.so" 1 "$nf: SQLBrowseConnect"
done
# An expected export must be a non-absolute FUNC that dlsym finds: glibc's lookup passes over a
# HIDDEN or INTERNAL symbol and one whose value is 0, while it treats a nonzero SHN_ABS value as
# an absolute address. Each refusal has a case; a PROTECTED function is found and passes.
rm -f "$tmp/symbol.so"; [[ ! -f "$tmp/base.so" ]] || \
  patch "$tmp/base.so" "$tmp/symbol.so" dynsym SQLBrowseConnect shndx 0xfff1
check "a nonzero expected FUNC with SHN_ABS fails" "$pol" "$host" "$tmp/symbol.so" 1 \
  "$nf: SQLBrowseConnect (FUNC, SHN_ABS)"
for c in "other 2:of HIDDEN visibility:1:(FUNC, HIDDEN)" "other 1:of INTERNAL visibility:1:(FUNC, INTERNAL)" \
  "value 0:at value 0:1:(FUNC, value 0)" "other 3:of PROTECTED visibility:0:"; do
  IFS=: read -r change what want kind <<<"$c"; read -r -a change <<<"$change"
  rm -f "$tmp/symbol.so"; [[ ! -f "$tmp/base.so" ]] || patch "$tmp/base.so" "$tmp/symbol.so" dynsym SQLBrowseConnect "${change[@]}"
  if ((want == 0)); then check "an expected export $what passes" "$pol" "$host" "$tmp/symbol.so" 0 "fits the Linux policy"
  else check "an expected export $what fails" "$pol" "$host" "$tmp/symbol.so" 1 "$nf: SQLBrowseConnect $kind"; fi
done
# A library linked the way a future release might be: DT_FLAGS_1 with NOW and an extra hash table.
# Nothing in it changes which library loads, so it must pass (DT_FLAGS is covered by the patched
# copy above).
lib benign "$pol/expected-exports.txt" -Wl,-z,now -Wl,--hash-style=both
check "a library with BIND_NOW and both hash tables passes" "$pol" "$host" "$tmp/benign.so" 0 "fits the Linux policy"
lib execstack "$pol/expected-exports.txt" -Wl,-z,execstack
check "an executable stack fails" "$pol" "$host" "$tmp/execstack.so" 1 "stack: the driver's PT_GNU_STACK is executable (RWE)"
lib debug "$pol/expected-exports.txt" -g
check "a .debug_info section fails" "$pol" "$host" "$tmp/debug.so" 1 "debug: the driver has debug sections: .debug_"
[[ ! -f "$tmp/debug.so" ]] || objcopy --remove-section=.debug_aranges --remove-section=.debug_abbrev --remove-section=.debug_line \
  --remove-section=.debug_str --remove-section=.debug_line_str --rename-section .debug_info=.zdebug_info "$tmp/debug.so" "$tmp/zdebug.so"
check "a .zdebug_info section fails" "$pol" "$host" "$tmp/zdebug.so" 1 "debug: the driver has debug sections: .zdebug_info"
# A version need no symbol carries: a stub libc that defines GLIBC_ABI_DT_RELR, and a library with
# relative relocations packed against it.
mkdir -p "$tmp/stub"
printf 'void stub_function(void) {}\n' > "$tmp/stub/stub.c"
printf 'GLIBC_2.2.5 { global: stub_function; };\nGLIBC_ABI_DT_RELR { };\n' > "$tmp/stub/stub.map"
printf 'void stub_function(void);\nstatic int x;\nint *p = &x, *q = &x;\nvoid relr_use(void) { stub_function(); }\n' > "$tmp/relr-data.c"
if out="$(gcc -shared -nostdlib -fPIC -o "$tmp/stub/libc.so.6" -Wl,-soname,libc.so.6 -Wl,--version-script="$tmp/stub/stub.map" "$tmp/stub/stub.c" 2>&1)"; then
  lib relr "$pol/expected-exports.txt" "$tmp/relr-data.c" -nostdlib "$tmp/stub/libc.so.6" -Wl,-z,pack-relative-relocs
  if [[ -f "$tmp/relr.so" ]] && ! readelf -VW "$tmp/relr.so" | grep -q 'Name: GLIBC_ABI_DT_RELR'; then
    n=$((n+1)); echo "ok $n - a version need no symbol carries fails # SKIP this linker does not pack relative relocations"
  else
    check "a version need no symbol carries fails" "$pol" "$host" "$tmp/relr.so" 1 "glibc: the driver requires GLIBC_ABI_DT_RELR, which is not a released"
  fi
else fail "building the stub libc failed: $out"; fi
if gcc -c -fPIC -o "$tmp/object.o" "$tmp/base.c"; then :; fi
check "a relocatable object is not a shared object" "$pol" "$host" "$tmp/object.o" 1 "machine: the driver is not a shared object (e_type 1)"
printf 'not an ELF file\n' > "$tmp/text.so"
check "a file that is not ELF fails" "$pol" "$host" "$tmp/text.so" 1 "machine: the driver is not a 64-bit little-endian ELF file"
# Each binutils tool the rules read with must be on PATH, or the run fails naming it.
for missing in readelf od; do
  d="$tmp/without-$missing"; mkdir -p "$d"; ln -s "$(command -v bash)" "$d/"
  for t in readelf od; do [[ "$t" == "$missing" ]] || ln -s "$(command -v "$t")" "$d/"; done
  out="$(env PATH="$d" "$(command -v bash)" "$policy_tool" "$pol" "$header" "$host" "$tmp/base.so" 2>&1)" && status=0 || status=$?
  if [[ "$status" == 1 && "$out" == "tools: $missing is required and was not found on PATH" ]]; then
    pass "without $missing on PATH, the rules fail naming it"; else fail "without $missing (exit $status; $out)"; fi
done
rm "$pol/allowed-needed.txt"
check "a missing policy file fails" "$pol" "$host" "$tmp/base.so" 1 "policy: allowed-needed.txt is missing"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
