#!/usr/bin/env bash
# The Linux loading policy of verification, with the policy as an argument: verify_linux.sh passes
# the tree's policy/linux/, the tests their own.
#
#   elf_policy.sh <policy-dir> <interface-header> <platform> <driver>
#
# Each rule fails with its name and the difference, in this order:
#   machine       a 64-bit little-endian ELF shared object for the platform's machine, whose program
#                 headers readelf counts as glibc does: e_phentsize 56, and e_phnum not PN_XNUM
#   dynamic-view  the .dynamic section, which readelf reads for the rules below, is the table the
#                 loader reads at PT_DYNAMIC's address, through the PT_LOAD that maps it, and it
#                 ends with DT_NULL; and each PT_LOAD starts at or above the page-rounded end of
#                 the one before it, so they are in address order and share no page
#   strings       one .dynsym row of type DYNSYM links to the one .dynstr row, of type STRTAB, both
#                 in readelf -SW's ten-field form, whose padded Name column reads ".dynstr " as
#                 .dynstr too; so readelf -tW pins the name: that row is the only section named
#                 exactly .dynstr (readelf -d and -V read names from such a STRTAB); and no
#                 string in that .dynstr holds a space, a control character or a literal @, which
#                 would split a name, end a list early or look like readelf's symbol-version suffix
#   exports       readelf -V lists at most one version-definition section, named .gnu.version_d,
#                 whose readelf -SW row has type VERDEF and links to .dynstr; the defined dynamic
#                 symbols (readelf --dyn-syms) equal expected-exports.txt, both ways, and each
#                 expected one is defined once in the default version, as a non-absolute function
#                 dlsym finds
#   entry-points  every name in the header's REDSHIFT_ODBC_ENTRY_POINTS is in expected-exports.txt
#   needed        every DT_NEEDED entry (readelf -d) names a library in allowed-needed.txt; an
#                 entry readelf cannot name fails
#   search-path   no DT_RPATH or DT_RUNPATH
#   textrel       no DT_TEXTREL, and no TEXTREL in DT_FLAGS
#   dynamic       every dynamic entry's tag is one that cannot change which library loads: the four
#                 that name another object for the loader (DT_AUDIT, DT_DEPAUDIT, DT_AUXILIARY,
#                 DT_FILTER) fail, as does any tag not on the list below
#   flags         no DF_1_GLOBAL, DF_1_LOADFLTR or DF_1_PIE in DT_FLAGS_1
#   stack         one PT_GNU_STACK (readelf -l), not executable
#   debug         no .debug_* or .zdebug_* section (readelf -S): the driver is embedded unchanged
#   glibc         readelf -V lists exactly one version-needs section, named .gnu.version_r, whose
#                 readelf -SW row has type VERNEED and links to .dynstr; it lists one file of
#                 version needs for each of DT_VERNEEDNUM, and a name for each need of the chain
#                 read from that section header's offset (a section of at most 1 MiB, at most 4096
#                 files and 65536 needs); every GLIBC_ version the driver needs (so also a need no
#                 symbol carries) is numeric and at most glibc-floor.txt
# Nothing in <driver> is executed: the binutils only read it.
set -euo pipefail
# readelf translates its headings and labels, which the rules parse: read them in the C locale.
export LC_ALL=C

(($# == 4)) || { echo "usage: elf_policy.sh <policy-dir> <interface-header> <platform> <driver>" >&2; exit 2; }
policy="$1" header="$2" platform="$3" elf="$4"
fail() { echo "$*" >&2; exit 1; }
lib_dir="${BASH_SOURCE[0]%/*}"            # without dirname, which a missing-tool case takes away
[[ "$lib_dir" != "${BASH_SOURCE[0]}" ]] || lib_dir=.
# shellcheck source=common.sh
. "$lib_dir/common.sh" # policy_lines
for t in readelf od; do command -v "$t" >/dev/null || fail "tools: $t is required and was not found on PATH"; done
lines() { policy_lines "$policy" "$1"; }
# listed <lines>: the lines of <lines> on one line, for a message.
listed() { printf '%s\n' "$1" | sed '/^$/d' | tr '\n' ' ' | sed 's/ $//'; }
case "$platform" in
# page: the largest page the platform's kernels use, which dynamic-view rounds a PT_LOAD's range to.
# aarch64 kernels use 4, 16 or 64 KiB, so 64 KiB covers every page a smaller one would; it also
# refuses an aarch64 driver whose PT_LOADs are only 4 KiB apart, which could not load on a 64 KiB
# host anyway (the loader requires each segment's address and offset to agree modulo the page).
linux_amd64) machine=62 machine_name=x86-64 page=0x1000 ;;    # EM_X86_64, one 4 KiB page size
linux_arm64) machine=183 machine_name=aarch64 page=0x10000 ;; # EM_AARCH64
*) fail "machine: $platform is not a Linux platform" ;;
esac
work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT

# machine: e_ident class and data, e_type ET_DYN, e_machine, from the 64-byte ELF header; then the
# program headers' size and count, which every rule below reads through readelf. With e_phnum
# PN_XNUM (0xffff), readelf takes the count from section 0's sh_info, while glibc reads 65535
# headers, so a PT_DYNAMIC or PT_LOAD past readelf's count would be the loader's alone; and glibc
# refuses an e_phentsize other than 56, which readelf reads all the same.
read -r -a ident <<<"$(od -An -v -tu1 -N64 -- "$elf" | tr '\n' ' ')"
((${#ident[@]} == 64)) && [[ "${ident[*]:0:6}" == "127 69 76 70 2 1" ]] ||
  fail "machine: the driver is not a 64-bit little-endian ELF file"
((ident[16] + 256 * ident[17] == 3)) || fail "machine: the driver is not a shared object (e_type $((ident[16] + 256 * ident[17])))"
((ident[18] + 256 * ident[19] == machine)) ||
  fail "machine: the driver is ELF machine $((ident[18] + 256 * ident[19])), not $machine_name ($machine) for $platform"
((ident[54] + 256 * ident[55] == 56)) ||
  fail "machine: the driver's program headers are $((ident[54] + 256 * ident[55])) bytes each (e_phentsize), not the 56 glibc reads"
((ident[56] + 256 * ident[57] != 0xffff)) ||
  fail "machine: the driver's e_phnum is PN_XNUM (0xffff): readelf takes the count from section 0," \
    "glibc reads 65535 headers"

# dynamic-view: readelf reads the dynamic table (for readelf -d, and the version tables it names for
# --dyn-syms) at the .dynamic section's offset, up to DT_NULL or the section's end; the loader reads
# it at PT_DYNAMIC's address, up to DT_NULL, from the bytes the PT_LOADs put there. The loader maps
# each PT_LOAD in turn over the pages from its address to its end (its file bytes, then zeros to
# memsz), a later one over an earlier one. So there must be one PT_DYNAMIC and one .dynamic, of
# type DYNAMIC (readelf -d reads no table from a NOBITS one), with the same offset and size, the
# offset readelf -d says it reads; one PT_LOAD that maps that address to that offset within its
# file bytes; and a DT_NULL in the table. And each PT_LOAD must start at or above the end of the
# last page of the one before it, so that none maps over another's bytes, those of this table or of
# any other the loader reads (.dynsym, .gnu.version). The other tables are read as readelf finds
# them: the symbols of every DYNSYM-typed section, with their names through each one's linked
# string table, not the loader's DT_SYMTAB, DT_STRTAB and DT_GNU_HASH (strings scans only the
# checked .dynsym row's); their @@V/@V suffixes from DT_VERSYM, DT_VERDEF and DT_VERNEED in
# this table, at the file offsets of the first PT_LOAD that holds each address;
# DT_NEEDED and DT_SONAME names through the section named .dynstr, not the loader's DT_STRTAB; and
# only readelf -V's lists (the version definitions exports sets aside, the needs glibc reads)
# through the .gnu.version_d and .gnu.version_r section headers. A crafted section table can hide,
# add or shift sections that readelf -SW does not list as the loader sees them. That and these
# differences between section headers and dynamic tags are accepted residuals for the signed,
# SHA-256-pinned driver.
readelf -lW -- "$elf" > "$work/segments" || fail "dynamic-view: readelf cannot read the driver's program headers"
readelf -SW -- "$elf" > "$work/sections" || fail "dynamic-view: readelf cannot read the driver's sections"
# section <name>: after readelf -SW's "[Nr]", the ELF64 rows this helper accepts have ten fields:
# Name Type Address Off Size ES Flg Lk Inf Al. The machine rule establishes ELF64, -W keeps the row
# unwrapped, and the current sections selected for offsets and links have a nonempty Flg column.
# Other row shapes are outside this helper under the accepted crafted-section-table residual above.
# Print "index name type offset size link" for each row whose Name field is <name>. That column is
# padded with spaces, so a section named ".dynstr " matches .dynstr too: this is not an exact-name
# match. For .dynstr, the readelf -tW check under strings enforces the exact name. For
# .gnu.version_d and .gnu.version_r, readelf -V's headings need the exact name of the section
# readelf reads, but not of this row (the crafted-section-table residual above).
section() {
  awk -v value="$1" '{
    idx = $0
    if (!sub(/^ *\[ *[0-9]+\] +/, "")) next
    sub(/^ *\[/, "", idx); sub(/\].*$/, "", idx); gsub(/ /, "", idx)
    if (NF == 10 && $1 == value) print idx, $1, $2, $4, $5, $8
  }' "$work/sections"
}
# one_row <rule> <name> <type> [<link>]: the one ten-field row section() matches for <name> (by the
# padded Name column, as above), which must be of <type> and, with <link>, link to section <link>
# (the .dynstr row); into $row.
one_row() {
  local r="$1" name="$2" type="$3" link="${4:-}" t l
  row="$(section "$name")"
  [[ -n "$row" && "$row" != *$'\n'* ]] ||
    fail "$r: readelf -SW lists $(printf '%s' "$row" | grep -c .) ten-field rows named $name, not one"
  read -r _ _ t _ _ l <<<"$row"
  [[ "$t" == "$type" ]] || fail "$r: the $name section is of type $t, not $type"
  [[ -z "$link" || "$l" == "$link" ]] || fail "$r: the $name section links to section $l, not to .dynstr (section $link)"
}
# Each value is checked to be a hex number, as readelf printed it, before bash computes with it,
# since arithmetic would expand anything else (a section name is the driver's text); at most 15
# digits, so that no sum below overflows. hex <rule> <prefix> <value>...: <prefix> is what readelf
# prints before a number, 0x for -l and nothing for -S.
hex() {
  local r="$1" p="$2" v; shift 2
  for v; do
    [[ "$v" =~ ^${p}0*[0-9a-fA-F]{1,15}$ ]] || fail "$r: readelf printed '$v' where a number belongs"
  done
}
# bytes <rule> <section> <offset> <size> <output>: one decimal byte per line.
bytes() {
  local r="$1" name="$2" off="$3" size="$4" out="$5" where
  printf -v where '0x%x' "$off"
  od -An -v -tu1 -j "$off" -N "$size" -- "$elf" | tr -s ' \n' '\n\n' > "$out" ||
    fail "$r: od cannot read the driver's $name at offset $where"
}
seg="$(awk '$1 == "DYNAMIC" { print $2, $3, $5 }' "$work/segments")" # offset address size
sec="$(section .dynamic)"
[[ -n "$seg" && "$seg" != *$'\n'* && -n "$sec" && "$sec" != *$'\n'* ]] ||
  fail "dynamic-view: the driver needs exactly one PT_DYNAMIC and one .dynamic section"
read -r off addr size <<<"$seg"; read -r _ _ s_type s_off s_size _ <<<"$sec"
hex dynamic-view 0x "$off" "$addr" "$size"; hex dynamic-view "" "$s_off" "$s_size"
[[ "$s_type" == DYNAMIC ]] || fail "dynamic-view: the .dynamic section is of type $s_type, not DYNAMIC"
s_off="0x$s_off" s_size="0x$s_size"
((off == s_off && size == s_size)) ||
  fail "dynamic-view: PT_DYNAMIC (offset $off, size $size) is not the .dynamic section (offset $s_off, size $s_size)"
# The end of the last page of the PT_LOAD before, from its address to the end of its memory or file
# bytes, whichever is larger: each PT_LOAD must start at or above it, so the PT_LOADs are in address
# order (glibc sizes the range it reserves from the last one) and no two share a page.
end="" mapped=""
while read -r l_off l_addr l_size l_mem; do
  hex dynamic-view 0x "$l_off" "$l_addr" "$l_size" "$l_mem"
  ((l_mem >= l_size)) || l_mem="$l_size" # the loader maps the file bytes even past a smaller memsz
  [[ -z "$end" ]] || ((l_addr >= end)) ||
    fail "dynamic-view: the PT_LOAD at $l_addr starts below $(printf '0x%x' "$end"), where the pages of the" \
      "PT_LOAD before it end: the PT_LOADs must be in address order, none on another's pages"
  end=$(((l_addr + l_mem + page - 1) & -page))
  if ((l_addr <= addr && addr + size <= l_addr + l_size && addr - l_addr == off - l_off)); then mapped=1; fi
done < <(awk '$1 == "LOAD" { print $2, $3, $5, $6 }' "$work/segments")
[[ -n "$mapped" ]] || fail "dynamic-view: no PT_LOAD maps PT_DYNAMIC's address $addr to its offset $off, so the loader reads other bytes"
readelf -dW -- "$elf" > "$work/dynamic" || fail "dynamic-view: readelf cannot read the driver's dynamic section"
# readelf -d must say it read the table at that offset: it reads none from a .dynamic of size 0.
first="$(awk 'NF { print; exit }' "$work/dynamic")"
re='^Dynamic section at offset 0x([0-9a-f]{1,15}) contains [0-9]+ entr(y|ies):$'
[[ "$first" =~ $re ]] && ((0x${BASH_REMATCH[1]} == s_off)) ||
  fail "dynamic-view: readelf -d does not read the table at the .dynamic section's offset $s_off: ${first:-no output}"
# DT_NULL from the table's bytes, not readelf's text, where a string such as the SONAME can print a
# line of its own: some 16-byte entry's d_tag, its first 8 bytes, must be all zeros.
bytes dynamic-view .dynamic "$((s_off))" "$((s_size))" "$work/dynamic-bytes"
awk 'NF { if (n % 16 < 8 && $1 != 0) set[int(n / 16)] = 1; n++ }
  END { for (e = 0; e < int(n / 16); e++) if (!(e in set)) exit 0; exit 1 }' "$work/dynamic-bytes" ||
  fail "dynamic-view: the .dynamic section has no DT_NULL, so the loader reads entries past it"

# strings: readelf prints symbol names with their spaces, and readelf -V and -d with their control
# characters too, and the rules split its lines at blanks, so "UND evil" would read as an undefined
# name and a version name with a newline would end readelf -V's list. A literal @ would look like
# the version suffix readelf adds to a dynamic-symbol name. readelf --dyn-syms reads the names of
# .dynsym's symbols through its link; readelf -d and -V through a STRTAB section named exactly
# .dynstr, flags or not (the first one; with none, DT_STRTAB). So the ten-field .dynsym row must
# link to the ten-field .dynstr row, and that row must be the only section named exactly .dynstr:
# readelf -tW prints each name on a line of its own, so a name padded with spaces, which reads the
# same in readelf -SW's columns, does not pass for it. Every byte of every string must be above
# space (0x20), not @ (0x40), and not DEL (0x7f); the message prints the first string that breaks
# this, with control bytes in octal.
one_row strings .dynstr STRTAB
read -r str_index _ _ str_off str_size _ <<<"$row"
one_row strings .dynsym DYNSYM "$str_index"
readelf -tW -- "$elf" > "$work/names" || fail "strings: readelf cannot read the driver's section names"
named="$(awk '/^  \[ *[0-9]+\] / { i = $0; sub(/^  \[ */, "", i); sub(/\].*$/, "", i)
    sub(/^  \[ *[0-9]+\] /, ""); if ($0 == ".dynstr") { printf "%s%s", c, i; c = " " } }' "$work/names")"
[[ "$named" == "$str_index" ]] ||
  fail "strings: the sections named exactly .dynstr are ${named:-none}, not only the checked row $str_index," \
    "so readelf -d and -V may print names from another"
hex strings "" "$str_off" "$str_size"
bytes strings .dynstr "$((0x$str_off))" "$((0x$str_size))" "$work/string-bytes"
bad="$(awk 'NF { if ($1 == 0) { if (bad) exit; s = ""; next }
    if ($1 <= 32 || $1 == 127) { bad = 1; s = s sprintf("\\%03o", $1) }
    else { if ($1 == 64) bad = 1; s = s sprintf("%c", $1) } }
    END { if (bad) print s }' "$work/string-bytes")"
[[ -z "$bad" ]] ||
  fail "strings: the driver's .dynstr holds a name with a space, a control character or '@': $bad"

# exports, both ways, by name without the version: the dynamic symbols that are defined and not
# local, less the absolute ones that name the driver's own version definitions (V1, in a library
# versioned V1), as readelf reads them (the residual under dynamic-view). Then each expected name
# must have exactly one definition in the default version, name@@V or unversioned, and it must be a
# function dlsym finds: a FUNC of DEFAULT or PROTECTED visibility whose value is not 0 and whose
# section index is not SHN_ABS. Glibc's lookup passes over a HIDDEN or INTERNAL symbol and one at 0
# that is neither thread-local nor SHN_ABS; it treats a nonzero SHN_ABS value as an absolute
# address, which need not point to code in the driver.
# readelf -V finds version definitions by section type, including rows section() does not accept.
# Its headings therefore own the count and exact name. If one exists, its ten-field row supplies
# the type and link used below.
lines expected-exports.txt | sort -u > "$work/want"
readelf --dyn-syms -W -- "$elf" > "$work/symbols" || fail "exports: readelf cannot read the driver's dynamic symbols"
readelf -VW -- "$elf" > "$work/versions" || fail "exports: readelf cannot read the driver's version sections"
read -r definition_sections named_definitions need_sections named_needs <<<"$(awk '
  /^Version definition section / { d++; if ($4 == "\047.gnu.version_d\047") dn++ }
  /^Version needs section / { n++; if ($4 == "\047.gnu.version_r\047") nn++ }
  END { print d + 0, dn + 0, n + 0, nn + 0 }' "$work/versions")"
((definition_sections <= 1 && definition_sections == named_definitions)) ||
  fail "exports: readelf -V must list at most one version-definition section, named .gnu.version_d"
((!definition_sections)) || one_row exports .gnu.version_d VERDEF "$str_index"
defs=" $(awk '/^Version definition section/ { on = 1; next } /^[^ ]/ { on = 0 }
  on && NF >= 2 && $(NF - 1) == "Name:" { print $NF }' "$work/versions" | tr '\n' ' ')"
# A row: "N: value size type bind visibility [other] ndx name", an undefined versioned name
# followed by "(index)". binutils 2.41 prints an st_other note such as [VARIANT_PCS] before ndx;
# 2.30 prints it after the name, so such a row fails there, as an extra export, never a pass.
awk -v defs="$defs" -v kinds="$work/kinds" '$1 ~ /^[0-9]+:$/ && $5 != "LOCAL" {
    name = $NF; ndx = $(NF - 1)
    if (name ~ /^\([0-9]+\)$/) { name = $(NF - 1); ndx = $(NF - 2) }
    plain = name; sub(/@.*/, "", plain)
    if (ndx == "UND" || (ndx == "ABS" && index(defs, " " plain " "))) next
    print plain
    what = $4; if (ndx == "ABS") what = what ", SHN_ABS"
    if ($6 != "DEFAULT" && $6 != "PROTECTED") what = what ", " $6
    if ($2 ~ /^0+$/ && $4 != "TLS") what = what ", value 0"
    kind = (name ~ /@/ && name !~ /@@/) ? "hidden" : (what == "FUNC" ? "function" : "other")
    print plain, kind, name " (" what ")" > kinds
  }' "$work/symbols" | sort -u > "$work/got"
extra="$(comm -13 "$work/want" "$work/got")" missing="$(comm -23 "$work/want" "$work/got")"
[[ -z "$extra$missing" ]] ||
  fail "exports: the driver's exports differ from expected-exports.txt; extra: $(listed "$extra"); missing: $(listed "$missing")"
not_one_default_function="$(awk 'NR == FNR { want[$1]; next }
  $1 in want { d = $0; sub(/^[^ ]+ [^ ]+ /, "", d); seen[$1] = seen[$1] ", " d; kinds_seen[$1 " " $2]
    if ($2 != "hidden") defaults[$1]++ }
  END { for (n in want) if (!((n " function") in kinds_seen) || defaults[n] > 1) print substr(seen[n], 3) }' \
  "$work/want" "$work/kinds" | sort)"
[[ -z "$not_one_default_function" ]] ||
  fail "exports: expected exports that are not one non-absolute function of the default version:" \
    "${not_one_default_function//$'\n'/; }"

# entry-points: X(Name) in the macro's continuation lines is SQLName.
[[ -f "$header" && -r "$header" ]] || fail "entry-points: the interface header $header is missing or unreadable"
awk '/^#define REDSHIFT_ODBC_ENTRY_POINTS\(X\)/ { on = 1 }
  on { s = $0; while (match(s, /X\([A-Za-z0-9_]+\)/)) { print "SQL" substr(s, RSTART + 2, RLENGTH - 3); s = substr(s, RSTART + RLENGTH) } }
  on && !/\\$/ { exit }' "$header" | sort -u > "$work/entries"
[[ -s "$work/entries" ]] || fail "entry-points: $header has no REDSHIFT_ODBC_ENTRY_POINTS(X) list"
absent="$(comm -23 "$work/entries" "$work/want")"
[[ -z "$absent" ]] || fail "entry-points: the interface's entry points are not all in expected-exports.txt; missing: $(listed "$absent")"

# needed, search-path, textrel, from the dynamic section.
lines allowed-needed.txt | sort -u > "$work/allowed"
# readelf prints the raw value, not "[name]", for a name it cannot find in the string table; such an
# entry fails rather than passing unnamed.
awk '/\(NEEDED\)/ { if (!match($0, /Shared library: \[[^]]+\]$/)) { bad = 1; exit }
  print substr($0, RSTART + 17, RLENGTH - 18) } END { exit bad }' "$work/dynamic" | sort -u > "$work/needed" ||
  fail "needed: the driver has a DT_NEEDED entry whose name readelf cannot read: $(grep '(NEEDED)' "$work/dynamic" | grep -v 'Shared library: \[' | head -1 | sed 's/^ *//')"
unexpected="$(comm -23 "$work/needed" "$work/allowed")"
[[ -z "$unexpected" ]] || fail "needed: the driver needs libraries not in allowed-needed.txt: $(listed "$unexpected")"
paths="$(awk '/\((RPATH|RUNPATH)\)/ { $1 = ""; print }' "$work/dynamic")"
[[ -z "$paths" ]] || fail "search-path: the driver has a library search path:$(listed "$paths")"
! grep -Eq '\(TEXTREL\)|\(FLAGS\).*TEXTREL' "$work/dynamic" || fail "textrel: the driver has text relocations (DT_TEXTREL)"

# dynamic: the tags the 2.3.0.0 drivers use and the rest of the base ABI, none of which can name
# another object for the loader; DT_SYMBOLIC (0x10) and its DT_FLAGS bit only change how the
# driver's own symbols resolve, as RTLD_DEEPBIND already does. Exactly three tags are left out on
# purpose, because the rules above name them first and give the better message: DT_RPATH (0xf),
# DT_TEXTREL (0x16) and DT_RUNPATH (0x1d). Nothing else may change how the driver loads.
allowed_tags=" 0x0 0x1 0x2 0x3 0x4 0x5 0x6 0x7 0x8 0x9 0xa 0xb 0xc 0xd 0xe 0x10 0x11 0x12 0x13 0x14"
allowed_tags+=" 0x15 0x17 0x18 0x19 0x1a 0x1b 0x1c 0x1e 0x20 0x21 0x22 0x23 0x24 0x25 0x6ffffef5"
allowed_tags+=" 0x6ffffef6 0x6ffffef7 0x6ffffff0 0x6ffffff9 0x6ffffffa 0x6ffffffb 0x6ffffffc"
allowed_tags+=" 0x6ffffffd 0x6ffffffe 0x6fffffff "
# A processor-specific tag (0x70000000 and up) only tells the loader about this object's own code:
# DT_AARCH64_BTI_PLT and DT_AARCH64_PAC_PLT for branch protection, DT_X86_64_PLT for CET. The three
# at the top of that range that name another object (DT_FILTER 0x7fffffff, DT_USED 0x7ffffffe and
# DT_AUXILIARY 0x7ffffffd) are not covered by it and fail with the rest.
unexpected="$(awk -v allowed="$allowed_tags" '$1 ~ /^0[xX][0-9a-fA-F]+$/ {
    tag = tolower($1); sub(/^0x0*/, "0x", tag); if (tag == "0x") tag = "0x0"
    # The processor-specific range, by the shape of the tag rather than its value, since strtonum
    # is a GNU awk extension: 0x7 and seven more hex digits, written out so the rule needs no
    # interval support either, less the three at the top of the range.
    if (tag ~ /^0x7[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$/ &&
        tag != "0x7ffffffd" && tag != "0x7ffffffe" && tag != "0x7fffffff") next
    if (index(allowed, " " tag " ") == 0) {
      match($0, /\([^)]*\)/)  # the type readelf printed: (AUXILIARY), or (<unknown>: 30)
      print $1, substr($0, RSTART, RLENGTH)
    }
  }' "$work/dynamic")"
[[ -z "$unexpected" ]] ||
  fail "dynamic: the driver has dynamic entries that can change how it loads: $(listed "$unexpected")"

# flags: DF_1_GLOBAL would put the driver in the global symbol scope, defeating the loader's
# RTLD_LOCAL isolation; DF_1_LOADFLTR loads filtees at once; DF_1_PIE marks a position-independent
# executable, not a library.
bad_flags="$(awk '$2 == "(FLAGS_1)" { for (i = 4; i <= NF; i++) if ($i ~ /^(GLOBAL|LOADFLTR|PIE)$/) print $i }' "$work/dynamic")"
[[ -z "$bad_flags" ]] || fail "flags: the driver sets DT_FLAGS_1 bits that change how it loads: $(listed "$bad_flags")"

# stack: the flags of the one PT_GNU_STACK, between the sizes and the alignment.
flags="$(awk '$1 == "GNU_STACK" { f = ""; for (i = 7; i < NF; i++) f = f $i; print f }' "$work/segments")"
[[ -n "$flags" && "$flags" != *$'\n'* ]] || fail "stack: the driver has no single PT_GNU_STACK, so its stack may be executable"
[[ "$flags" != *E* ]] || fail "stack: the driver's PT_GNU_STACK is executable ($flags)"

# debug.
debug="$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\.z?debug_/) print $i }' "$work/sections")"
[[ -z "$debug" ]] || fail "debug: the driver has debug sections: $(listed "$debug")"

# glibc: every version need's name in .gnu.version_r, compared with the floor number by number.
floor="$(lines glibc-floor.txt)"
[[ "$floor" =~ ^[0-9]+(\.[0-9]+)+$ ]] || fail "policy: glibc-floor.txt must hold one version such as 2.28"
newer() { # newer <a> <b>: a is a later version than b
  local -a a b; local i
  IFS=. read -r -a a <<<"$1"; IFS=. read -r -a b <<<"$2"
  for ((i = 0; i < ${#a[@]} || i < ${#b[@]}; i++)); do
    ((10#${a[i]:-0} == 10#${b[i]:-0})) || return $((10#${a[i]:-0} < 10#${b[i]:-0}))
  done
  return 1
}
# Scan readelf's version-need lists once. The heading count above includes every section readelf
# found by VERNEED type, including a row section() does not accept.
awk '/^Version needs section/ { on = 1; next } /^[^ ]/ { on = 0 }
  on && $4 == "File:" { print "file" }
  on && $2 == "Name:" { print "name", $3 }' "$work/versions" > "$work/version-needs"
read -r files names <<<"$(awk '$1 == "file" { f++ } $1 == "name" { n++ }
  END { print f + 0, n + 0 }' "$work/version-needs")"
((need_sections == 1 && named_needs == 1)) ||
  fail "glibc: readelf -V must list exactly one version-needs section, named .gnu.version_r"
one_row glibc .gnu.version_r VERNEED "$str_index"
read -r _ _ _ vr_off vr_size _ <<<"$row"
# readelf -V starts at .gnu.version_r's section-header offset. It must list one file for each of
# DT_VERNEEDNUM, and at least one where there is a DT_VERNEED. A signed, SHA-256-pinned file whose
# DT_VERNEED points at another self-consistent chain remains part of dynamic-view's accepted
# section-header-versus-dynamic-tag residual.
read -r verneed verneednum <<<"$(awk '$2 == "(VERNEED)" { v = 1 } $2 == "(VERNEEDNUM)" { n = $3 }
  END { print v + 0, n + 0 }' "$work/dynamic")"
((files == verneednum && files >= verneed)) ||
  fail "glibc: readelf -V reads $files version-need entries, where DT_VERNEEDNUM is $verneednum" \
    "and DT_VERNEED is $( ((verneed)) && echo set || echo absent): the needs cannot be read"
# readelf stops after .gnu.version_r's sh_info files and each file's vn_cnt needs, while the chain
# from the section-header offset follows vn_aux, vn_next and vna_next to zero. It must print a name
# for every need in that chain. vn_aux is at 8; vn_next and vna_next are at 12; each is a 4-byte
# little-endian value. The section may be at most 1 MiB, and at most 4096 files and 65536 needs
# are read.
hex glibc "" "$vr_off" "$vr_size"
((0x$vr_size <= 0x100000)) ||
  fail "glibc: the .gnu.version_r section exceeds the 1 MiB limit ($((0x$vr_size)) bytes)"
bytes glibc .gnu.version_r "$((0x$vr_off))" "$((0x$vr_size))" "$work/version-need-bytes"
chain="$(awk 'function u32(o) { return b[o] + 256 * (b[o + 1] + 256 * (b[o + 2] + 256 * b[o + 3])) }
    NF { b[n++] = $1 }
    END {
      for (e = 0; ; e += u32(e + 12)) {
        if (e + 16 > n) { print "file outside"; exit }
        if (++f > 4096) { print "too many files"; exit }
        for (a = e + u32(e + 8); ; a += u32(a + 12)) {
          if (a + 16 > n) { print "need outside"; exit }
          if (++k > 65536) { print "too many needs"; exit }
          if (!u32(a + 12)) break
        }
        if (!u32(e + 12)) break
      }
      print k " needs"
    }' "$work/version-need-bytes")"
case "$chain" in
  "file outside") fail "glibc: the version-need file chain does not end inside the .gnu.version_r section" ;;
  "need outside") fail "glibc: a version-need auxiliary chain does not end inside the .gnu.version_r section" ;;
  "too many files") fail "glibc: the version-need chain exceeds the 4096-file limit" ;;
  "too many needs") fail "glibc: the version-need chain exceeds the 65536-need limit" ;;
esac
[[ "$chain" == "$names needs" ]] ||
  fail "glibc: readelf -V prints $names version-need names, where the chain read from" \
    ".gnu.version_r's section-header offset holds $chain"
awk '$1 == "name" { print $2 }' "$work/version-needs" | sort -u > "$work/needs"
highest="" above=()
while read -r v; do
  [[ "$v" == GLIBC_* ]] || continue
  [[ "${v#GLIBC_}" =~ ^[0-9]+(\.[0-9]+)*$ ]] || fail "glibc: the driver requires $v, which is not a released glibc version"
  if [[ -z "$highest" ]] || newer "${v#GLIBC_}" "$highest"; then highest="${v#GLIBC_}"; fi
  ! newer "${v#GLIBC_}" "$floor" || above+=("$v")
done < "$work/needs"
((${#above[@]} == 0)) ||
  fail "glibc: the driver requires ${above[*]}, newer than the floor $floor in glibc-floor.txt"
echo "the driver fits the Linux policy (newest glibc version required: ${highest:-none})"
