#!/usr/bin/env bash
# Tool tests for the rest of Windows verification from Linux: tools/lib/msi.py and tools/lib/cab.py
# with its LZX decoder, tools/lib/verify_windows.sh as verify-release.sh runs it, fetch-driver.sh's
# Windows branch and tools/lib/download.py. Every MSI and cabinet is written at test time
# (make_msi.py, make_lzx_cab.py), signed with a test PKI, and nothing is checked in. A well-formed
# MSI passes, its rows make a lock check-lock.sh accepts, and fetch-driver.sh takes the same files
# from it; each file-list, bound, compression, custom-action, product and decoder case fails at its
# rule. The real MSI's case runs once the lock under test names windows_amd64. Run by
# run-tool-tests.sh.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=verify-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/verify-test-lib.sh"
# A reader or decoder that loops fails its case, at a bound a whole verification fits in.
BOUNDED_SECONDS=60
# shellcheck source=bounded.sh
source "$(dirname "${BASH_SOURCE[0]}")/bounded.sh"
driver_dir="$repo/test/client/driver"
t="$(test_tree winmsi)"
header="$repo/src/transport/odbc_driver.hpp"
pki_cert wleaf ca "$(aws_subject_with OU 'Amazon Redshift')" codesign
msi_url="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0/AmazonRedshiftODBC64-2.3.0.0.msi"
dir="./PFiles64/Amazon%20Redshift%20ODBC%20Driver%20(x64)"

# The set's two DLLs as PE32+ images, the driver exporting the interface's entry points, and two
# files the MSI holds beside them, one larger than a 32 KiB frame.
entry_points="$(python3 -I -c 'import re, sys
text = open(sys.argv[1]).read()
block = re.search(r"#define REDSHIFT_ODBC_ENTRY_POINTS\(X\)((?:[^\n]*\\\n)*[^\n]*)", text).group(1)
print(" ".join("SQL" + n for n in re.findall(r"X\(([A-Za-z0-9_]+)\)", block)))' "$header")"
exported="$(printf '"%s", ' $entry_points)"
python3 -I "$driver_dir/make_pe.py" "$tmp/rsodbc.dll" "{\"trailing\": \"driver\",
  \"rdata\": {\"export_name\": \"rsodbc.dll\", \"exported\": [${exported%, }]}}"
python3 -I "$driver_dir/make_pe.py" "$tmp/crypto.dll" '{"trailing": "crypto"}'
driver_size="$(wc -c < "$tmp/rsodbc.dll" | tr -d ' ')"
head -c 70000 /dev/urandom > "$tmp/setup.bin"
printf 'not a PE image\n' > "$tmp/text"
file_row() { echo "[\"$1\", \"$2\", \"INSTALLDIR\", \"$3\"]"; }
files="$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll"), $(file_row crypto.dll crypto.dll "@$tmp/crypto.dll"),
  $(file_row setup rsodbcsetup.dll "@$tmp/setup.bin"), $(file_row ini amazon.redshiftodbc.ini 'x=1')"
mkdir -p "$t/policy/windows"
echo "Amazon%20Redshift%20ODBC%20Driver%20(x64)" > "$t/policy/windows/product.txt"
policy_rows=("driver  rsodbc.dll  rsodbc.dll  INSTALLDIR" "library  crypto.dll  crypto.dll  INSTALLDIR"
  "leave  setup  rsodbcsetup.dll  INSTALLDIR" "leave  ini  amazon.redshiftodbc.ini  INSTALLDIR")
printf '%s\n' "# <driver|library|leave> <File key> <long name> <Directory key>" "${policy_rows[@]}" > "$t/policy/windows/msi-files.txt"
# The loading policy the two DLLs meet: no import and no run-time name, so those rows are emptied,
# and the driver's exports. The environment lists are emptied too, since these DLLs hold none of
# the names the real ones list.
printf '%s\n' $entry_points > "$t/policy/windows/expected-exports.txt"
for f in allowed-imports runtime-names; do : > "$t/policy/windows/$f.txt"; done
for f in refused-environment known-environment ignored-strings; do : > "$t/policy/$f.txt"; done
# msi <name> [<more spec>] [<files>]: a signed MSI of the set, $tmp/<name>.msi.
msi() {
  python3 -I "$driver_dir/make_msi.py" "$tmp/$1.msi" "{\"files\": [${3:-$files}], \"sign\": {\"leaf\": \"$pki/wleaf.pem\",
    \"key\": \"$pki/wleaf.key\", \"chain\": [\"$pki/ca.pem\"]}${2:+, $2}}" || fail "make_msi.py writes $1.msi"
}
# verify <name>: verify_windows.sh on $tmp/<name>.msi, served by the stand-in curl with the notices.
serve="$tmp/serve"; mkdir -p "$serve"
cp "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "$serve/"
export FAKE_CURL_DIR="$serve"
verify() {
  cp "$tmp/$1.msi" "$serve/AmazonRedshiftODBC64-2.3.0.0.msi"
  rm -f "$FAKE_CURL_LOG"
  out_text="$(bounded env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" ${env_extra[@]+"${env_extra[@]}"} \
    bash "$t/tools/lib/verify_windows.sh" windows_amd64 2.3.0.0 "$t" "$header" "$pki/bundle.pem" 2>&1)" &&
    status=0 || status=$?
}
env_extra=()

# A well-formed MSI: its rows, a lock made of them, and fetch-driver.sh's files.
msi good
verify good
expect "a well-formed signed MSI passes" 0 "the notices equal the checked-in copies"
said="verify-release: windows_amd64: the set's 2 files meet the Windows loading policy"
said+=$'\n'"verify-release: windows_amd64: crypto.dll: 0 imports"
classified="verify-release: windows_amd64: the set's 0 scanned names are all classified"
if [[ "$out_text" == *"$said"* && "$out_text" == *"$classified"* ]]
then pass "it applies the loading policy to each file and the environment-name rule to the set"
else fail "it applies the loading policy and the environment rule: $out_text"
fi
msi_sha="$(sha "$tmp/good.msi")"
want_rows=("windows_amd64  driver  rsodbc.dll  $(sha "$tmp/rsodbc.dll")  $(sha "$tmp/rsodbc.dll")  $msi_url  $msi_sha  $dir/rsodbc.dll"
  "windows_amd64  library  crypto.dll  $(sha "$tmp/crypto.dll")  $(sha "$tmp/crypto.dll")  $msi_url  $msi_sha  $dir/crypto.dll")
missing=""
for r in "${want_rows[@]}"; do grep -qxF -- "$r" <<<"$out_text" || missing+=" [$r]"; done
if [[ -z "$missing" && "$(grep -c '^\*  notice  ' <<<"$out_text")" == 3 ]]; then
  pass "it prints a row per file of the set, each with the file's SHA-256, the MSI's and the extract path, and the notice rows"
else fail "it prints a row per file of the set, each with the file's SHA-256, the MSI's and the extract path, and the notice rows (missing:$missing; $out_text)"; fi
# With a minimum version for each Windows prerequisite the policy names, as driver_info.py needs.
{ printf '%s\n' "format 2" "version 2.3.0.0" "status development"
  grep -E '^(windows_amd64|\*) ' <<<"$out_text"
  awk '$1 == "windows" { print "windows_amd64  prerequisite  " $2 "  14.0.0.0" }' \
    "$t/policy/host-prerequisites.txt"; } > "$tmp/win.lock"
out_text="$(bash "$tools/check-lock.sh" "$tmp/win.lock" 2>&1)" && status=0 || status=$?
expect "check-lock.sh accepts a lock made of the printed rows" 0 "driver windows_amd64 rsodbc.dll"
mkdir -p "$tmp/mirror"; cp "$tmp/good.msi" "$tmp/mirror/$msi_sha"
fetch() { # fetch <lock> <out-dir>
  out_text="$(env RS_DRIVER_LOCK="$1" RS_DRIVER_MIRROR="$tmp/mirror" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
    bash "$t/tools/fetch-driver.sh" windows_amd64 "$2" 2>&1)" && status=0 || status=$?
}
fetch "$tmp/win.lock" "$tmp/fetched"
# Compared by SHA-256, as cmp may be missing.
if [[ "$status" == 0 && -f "$tmp/fetched/driver_info.h" && "$(sha "$tmp/fetched/rsodbc.dll")" == "$(sha "$tmp/rsodbc.dll")" &&
  "$(sha "$tmp/fetched/crypto.dll")" == "$(sha "$tmp/crypto.dll")" && "$(sha "$tmp/fetched/NOTICE")" == "$(sha "$odbc/NOTICE")" ]] &&
  grep -q 'X("crypto.dll", 518, "[0-9a-f]*", library_crypto_dll)' "$tmp/fetched/driver_info.h"; then
  pass "fetch-driver.sh's Windows branch takes the same files from the MSI and writes driver_info.h"
else fail "fetch-driver.sh's Windows branch takes the same files from the MSI and writes driver_info.h (exit $status; $out_text)"; fi
sed 's/^\(windows_amd64  library  crypto.dll  \)[0-9a-f]\{64\}  [0-9a-f]\{64\}/\1'"$(printf 'a%.0s' {1..64})  $(printf 'a%.0s' {1..64})"'/' \
  "$tmp/win.lock" > "$tmp/tampered.lock"
fetch "$tmp/tampered.lock" "$tmp/fetched-tampered"
expect "a file whose SHA-256 is not the lock's fails the fetch" 1 "crypto.dll has SHA-256 $(sha "$tmp/crypto.dll"), the lock expects aaaa"
[[ ! -e "$tmp/fetched-tampered/driver_info.h" ]] || fail "a failed fetch left driver_info.h"
sed 's|/crypto\.dll$|/other.dll|' "$tmp/win.lock" > "$tmp/elsewhere.lock"
fetch "$tmp/elsewhere.lock" "$tmp/fetched-elsewhere"
expect "a lock path the MSI has no file at fails the fetch" 1 "cannot take the set's files from the MSI"

# A renamed set, as a renaming lock records one: both files renamed, and the driver's last bytes
# patched, so its sha256 is the patched bytes' and its source-sha256 the MSI's.
at="$(python3 -I -c 'import sys; print(open(sys.argv[1], "rb").read().rindex(b"driver"))' "$tmp/rsodbc.dll")"
python3 -I -c 'import sys; d = bytearray(open(sys.argv[1], "rb").read()); d[int(sys.argv[3]) + 5] = ord("z")
open(sys.argv[2], "wb").write(d)' "$tmp/rsodbc.dll" "$tmp/rsodbc-renamed.dll" "$at"
g_driver=abcdefghijklmnop.dll g_crypto=qrstuvwxyz234567.dll
# The renamed set's hash: the SHA-256 of "<sha256>  <size>  <name>" per file, sorted by name.
w="$tmp/want-set"; mkdir "$w"; cp "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "$w/"
cp "$tmp/rsodbc-renamed.dll" "$w/$g_driver"; cp "$tmp/crypto.dll" "$w/$g_crypto"
want_set_sha="$(python3 -I -c 'import hashlib, os, sys
d = sys.argv[1]; data = {n: open(os.path.join(d, n), "rb").read() for n in os.listdir(d)}
m = "".join("%s  %d  %s\n" % (hashlib.sha256(b).hexdigest(), len(b), n)
            for n, b in sorted(data.items()))
print(hashlib.sha256(m.encode()).hexdigest())' "$w")"
# renamed_lock <out> <driver sha256> <patch's original bytes> [<crypto's rename row>]: win.lock renamed.
renamed_lock() {
  { sed "s/^\(windows_amd64  driver  rsodbc.dll  \)[0-9a-f]\{64\}/\1$2/" "$tmp/win.lock"
    echo "windows_amd64  rename  rsodbc.dll  $g_driver"
    if [[ -z "${4+set}" ]]; then echo "windows_amd64  rename  crypto.dll  $g_crypto"; fi
    echo "windows_amd64  patch  rsodbc.dll  $at  $3  64726976657a"
    echo "windows_amd64  name-seed  $msi_sha"
    echo "windows_amd64  set-hash  $want_set_sha"; } > "$1"
}
renamed_lock "$tmp/renamed.lock" "$(sha "$tmp/rsodbc-renamed.dll")" 647269766572
fetch "$tmp/renamed.lock" "$tmp/fetched-renamed"
set_names="$(cd "$tmp/fetched-renamed" && for f in *.dll; do printf '%s ' "$f"; done)" || true
if [[ "$status" == 0 && "$set_names" == "$g_driver $g_crypto " &&
  "$(sha "$tmp/fetched-renamed/$g_driver")" == "$(sha "$tmp/rsodbc-renamed.dll")" &&
  "$(sha "$tmp/fetched-renamed/$g_crypto")" == "$(sha "$tmp/crypto.dll")" ]] &&
  grep -q "X(\"$g_driver\", $driver_size, \"$(sha "$tmp/rsodbc-renamed.dll")\", driver)" \
    "$tmp/fetched-renamed/driver_info.h" &&
  grep -q "X(\"$g_crypto\", 518, \"$(sha "$tmp/crypto.dll")\", library_crypto_dll)" \
    "$tmp/fetched-renamed/driver_info.h" &&
  grep -qxF "#define REDSHIFT_DRIVER_SET_SHA256 \"$want_set_sha\"" \
    "$tmp/fetched-renamed/driver_info.h"; then
  pass "a renamed lock's fetch applies its rows and embeds each file under its generated name with the lock's sha256"
else fail "a renamed lock's fetch applies its rows and embeds each file under its generated name with the lock's sha256 (exit $status; [$set_names]; $out_text)"; fi
# The resource check of make test_driver's Windows branch, on images holding the renamed set's bytes
# and on one holding the MSI's driver bytes instead.
resources() { # resources <driver file>: the set's RCDATA list, as the resource script names it
  local f="$tmp/fetched-renamed" r=""
  for pair in "DRIVER $1" "LIBRARY_CRYPTO_DLL $f/$g_crypto" "NOTICE_LICENSE $f/LICENSE" "NOTICE_NOTICE $f/NOTICE" \
    "NOTICE_THIRD_PARTY_LICENSES $f/THIRD_PARTY_LICENSES"; do
    r+="${r:+, }[10, \"${pair%% *}\", 0, \"@${pair#* }\"]"
  done
  echo "[$r]"
}
python3 -I "$driver_dir/make_pe.py" "$tmp/renamed-image.dll" "{\"resources\": $(resources "$tmp/fetched-renamed/$g_driver")}"
python3 -I "$driver_dir/make_pe.py" "$tmp/original-image.dll" "{\"resources\": $(resources "$tmp/rsodbc.dll")}"
out_text="$(python3 -I "$driver_dir/embedded_resources.py" "$tmp/fetched-renamed/driver_info.h" "$tmp/renamed-image.dll" 2>&1)" &&
  status=0 || status=$?
expect "an image whose resources are the renamed set's bytes passes the resource check" 0 "holds the 5 files of the set"
out_text="$(python3 -I "$driver_dir/embedded_resources.py" "$tmp/fetched-renamed/driver_info.h" "$tmp/original-image.dll" 2>&1)" &&
  status=0 || status=$?
expect "an image holding the MSI's driver bytes fails the resource check, against the renamed bytes' SHA-256" 1 \
  "RCDATA \"DRIVER\" ($g_driver) is $driver_size bytes with SHA-256 $(sha "$tmp/rsodbc.dll");\
 driver_info.h records $driver_size bytes with $(sha "$tmp/rsodbc-renamed.dll")"
# Rows that do not verify against the MSI's files fail the fetch, and leave no header.
renamed_lock "$tmp/bad-original.lock" "$(sha "$tmp/rsodbc-renamed.dll")" 647269766579
fetch "$tmp/bad-original.lock" "$tmp/fetched-bad-original"
expect "a patch whose original bytes the MSI's file does not hold fails the fetch" 1 \
  "do not verify against the MSI's files: patch: rsodbc.dll at $at does not hold the recorded original bytes 647269766579"
[[ ! -e "$tmp/fetched-bad-original/driver_info.h" ]] || fail "a failed renamed fetch left driver_info.h"
renamed_lock "$tmp/bad-renamed.lock" "$(printf 'b%.0s' {1..64})" 647269766572
fetch "$tmp/bad-renamed.lock" "$tmp/fetched-bad-renamed"
expect "a renamed sha256 the patches do not give fails the fetch" 1 \
  "do not verify against the MSI's files: rename: rsodbc.dll's renamed SHA-256 is $(sha "$tmp/rsodbc-renamed.dll"), not the sha256 bbbb"
renamed_lock "$tmp/half.lock" "$(sha "$tmp/rsodbc-renamed.dll")" 647269766572 no-crypto-rename
fetch "$tmp/half.lock" "$tmp/fetched-half"
expect "a lock that renames only part of the set fails the fetch" 1 "the lock renames the set but has no rename row for crypto.dll"
{ sed "s/^\(windows_amd64  driver  rsodbc.dll  \)[0-9a-f]\{64\}/\1$(sha "$tmp/rsodbc-renamed.dll")/" "$tmp/win.lock"
  echo "windows_amd64  patch  rsodbc.dll  $at  647269766572  64726976657a"; } > "$tmp/patch-only.lock"
fetch "$tmp/patch-only.lock" "$tmp/fetched-patch-only"
expect "a lock with patch rows and no rename rows fails the fetch" 1 "the lock renames the set but has no rename row for rsodbc.dll"
sed "s/^windows_amd64  set-hash  .*/windows_amd64  set-hash  $msi_sha/" "$tmp/renamed.lock" \
  > "$tmp/set-hash.lock"
fetch "$tmp/set-hash.lock" "$tmp/fetched-set-hash"
name="a set-hash the renamed set does not give fails the fetch, leaving no header"
if [[ "$status" == 1 && ! -e "$tmp/fetched-set-hash/driver_info.h" &&
  "$out_text" == *"SHA-256 $want_set_sha, and the lock's set-hash is $msi_sha"* ]]
then pass "$name"; else fail "$name (exit $status; $out_text)"; fi

# The rules of the files step, each on an MSI built for it.
case_of() { # case_of <name> <spec> <needle> [<files>]
  msi "$1" "$2" "${4:-}"; verify "$1"; expect "$5" 1 "$3"
}
case_of product '"product": "Another Driver"' "files: the signed MSI is 'Another Driver' version '2.3.0.0'" "" "an MSI of another product fails"
case_of version '"product_version": "2.3.0.1"' "version '2.3.0.1', not 'Amazon Redshift ODBC Driver (x64)' version '2.3.0.0'" "" "an MSI of another version fails"
for table in AdminExecuteSequence AdminUISequence; do
  case_of "ca-$table" "\"custom_action\": \"$table\"" "files: $table runs the custom action SetTarget" "" "a custom action in $table fails"
done
case_of added "" "files: the MSI holds extra extra.dll INSTALLDIR, which policy/windows/msi-files.txt does not list" \
  "$files, $(file_row extra extra.dll x)" "a file added to the MSI fails"
case_of dropped "" "files: the MSI lacks ini amazon.redshiftodbc.ini INSTALLDIR, which policy/windows/msi-files.txt lists" \
  "$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll"), $(file_row crypto.dll crypto.dll "@$tmp/crypto.dll"), $(file_row setup rsodbcsetup.dll "@$tmp/setup.bin")" \
  "a file dropped from the MSI fails"
case_of bound '"break": ["cab-size"]' "cab: rsodbc.dll decodes to 67108865 bytes, over the 67108864-byte bound" "" "a file over the decoded bound fails before it is decoded"
case_of mszip '"compression": 1' "cab: folder 0's compression 1 is not LZX, the only one this tool reads (in cabinet #Data1.cab)" "" \
  "a cabinet compression the tool does not read fails, naming the cabinet"
case_of loose '"break": ["loose"]' "files: rsodbc.dll is stored outside the MSI's cabinets" "" "a file outside the cabinet fails"
case_of size '"break": ["file-size"]' \
  "files: rsodbc.dll decodes to $driver_size bytes, and the File table says $((driver_size + 1))" \
  "" \
  "a File table size the cabinet does not have fails"
case_of cab-extra '"break": ["cab-extra"]' "files: the cabinet holds Extra, which the File table does not name" "" "a cabinet file the File table does not name fails"
case_of not-pe "" "files: crypto.dll is not a PE32+ image for x86-64" \
  "$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll"), $(file_row crypto.dll crypto.dll "@$tmp/text"), $(file_row setup rsodbcsetup.dll "@$tmp/setup.bin"), $(file_row ini amazon.redshiftodbc.ini x)" \
  "a set file that is not a PE32+ image for x86-64 fails"
python3 -I "$driver_dir/make_msi.py" "$tmp/unsigned.msi" "{\"files\": [$files]}"
verify unsigned; expect "an unsigned MSI fails at the signer" 1 "signer: the MSI is not signed"
cp "$t/policy/windows/msi-files.txt" "$tmp/msi-files.txt"
echo "leave  license  LICENSE  INSTALLDIR" >> "$t/policy/windows/msi-files.txt"
case_of notice "" "files: the MSI holds LICENSE, named like a notice but not the MSI's own <notice>.txt copy" \
  "$files, $(file_row license LICENSE x)" "an MSI that holds a notice under its bare name fails"
cp "$tmp/msi-files.txt" "$t/policy/windows/msi-files.txt"
# The MSI's own .txt copies, as 2.3.1.0's MSI carries them, left in the MSI: each must have the checked-in
# notice's bytes, the release tag's copy.
txt_rows="" txt_files=""
for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
  echo "leave  $notice.txt  $notice.txt  INSTALLDIR" >> "$t/policy/windows/msi-files.txt"
  txt_files+=", $(file_row "$notice.txt" "$notice.txt" "@$odbc/$notice")"
done
msi txt-notices "" "$files$txt_files"; verify txt-notices
expect "an MSI whose LICENSE.txt, NOTICE.txt and THIRD_PARTY_LICENSES.txt are the release tag's passes" 0 \
  "the notices equal the checked-in copies"
# A checked-in notice that cannot be read stops the comparison, naming it: here NOTICE, moved away.
mv "$t/NOTICE" "$tmp/NOTICE.away"
verify txt-notices
expect "an MSI's .txt notice whose checked-in copy is missing fails, naming it" 1 \
  "files: the checked-in NOTICE cannot be read: No such file or directory"
mv "$tmp/NOTICE.away" "$t/NOTICE"
printf 'another notice text\n' > "$tmp/other-notice"
for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
  other="$files"
  for m in LICENSE NOTICE THIRD_PARTY_LICENSES; do
    if [[ "$m" == "$notice" ]]; then other+=", $(file_row "$m.txt" "$m.txt" "@$tmp/other-notice")"
    else other+=", $(file_row "$m.txt" "$m.txt" "@$odbc/$m")"; fi
  done
  case_of "txt-$notice" "" "files: the MSI's $notice.txt has SHA-256 $(sha "$tmp/other-notice"); the checked-in $notice" \
    "$other" "an MSI whose $notice.txt is not the release tag's fails, naming it"
done
cp "$tmp/msi-files.txt" "$t/policy/windows/msi-files.txt"
echo "driver  other  other.dll  INSTALLDIR" >> "$t/policy/windows/msi-files.txt"
env_extra=(FAKE_CURL=fail); verify good; env_extra=()
if [[ ! -e "$FAKE_CURL_LOG" ]]; then expect "a policy that marks two drivers fails before any download" 1 "policy: policy/windows/msi-files.txt must mark exactly one file driver"
else fail "the broken policy case downloaded"; fi
cp "$tmp/msi-files.txt" "$t/policy/windows/msi-files.txt"
echo "driver  rsodbc.dll" >> "$t/policy/windows/msi-files.txt"
env_extra=(FAKE_CURL=fail); verify good; env_extra=()
expect "a malformed msi-files.txt row fails" 1 "policy: a row of policy/windows/msi-files.txt is <driver|library|leave>"
cp "$tmp/msi-files.txt" "$t/policy/windows/msi-files.txt"
# The loading policy and the environment-name rule, on a set file built to break each.
cp "$t/policy/windows/expected-exports.txt" "$tmp/expected-exports.txt"
grep -v '^SQLFetch$' "$tmp/expected-exports.txt" > "$t/policy/windows/expected-exports.txt"
env_extra=(FAKE_CURL=fail); verify good; env_extra=()
if [[ ! -e "$FAKE_CURL_LOG" ]]; then
  expect "an expected-exports.txt without an entry point fails before any download" 1 \
    "entry-points: expected-exports.txt lacks the interface's entry points SQLFetch"
else fail "the expected-exports.txt case downloaded"; fi
cp "$tmp/expected-exports.txt" "$t/policy/windows/expected-exports.txt"
python3 -I "$driver_dir/make_pe.py" "$tmp/crypto-import.dll" '{"rdata": {"imports": ["A.dll"]}}'
python3 -I "$driver_dir/make_pe.py" "$tmp/crypto-env.dll" \
  '{"rdata": {"strings": [["a", "SSL_CERT_FILE"]]}}'
for c in import:"imports: crypto.dll: it imports A.dll, which allowed-imports.txt does not list" \
  env:"environment: the set's crypto.dll names SSL_CERT_FILE, which no policy list classifies"; do
  case_of "crypto-${c%%:*}" "" "verify-release: windows_amd64: ${c#*:}" \
    "$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll"),
    $(file_row crypto.dll crypto.dll "@$tmp/crypto-${c%%:*}.dll"),
    $(file_row setup rsodbcsetup.dll "@$tmp/setup.bin"),
    $(file_row ini amazon.redshiftodbc.ini x)" \
    "a set file that breaks the ${c%%:*} rule fails verification"
done
# The database's own refusals.
case_of db-pool '"pool_extra": "00"' "files: the MSI has no string pool this tool reads" "" "a string pool of part entries fails"
case_of db-codepage '"codepage": 437' "files: the string pool's code page 437 is not one this tool reads" "" "a string pool code page the tool does not read fails"
# The code pages read besides 1252: 65001 as UTF-8, and 0 (the neutral page) as cp1252.
msi db-utf8 '"codepage": 65001, "product": "Pilote \u00e9"'
msi db-neutral '"codepage": 0, "product": "Pilote \u00e9"'
out_text="$(python3 -I -B - "$tools/lib/msi.py" "$tmp/db-utf8.msi" "$tmp/db-neutral.msi" <<'EOF' 2>&1
import importlib.util, sys
spec = importlib.util.spec_from_file_location("msi", sys.argv[1]); msi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(msi)
print(" ".join(ascii(msi.product(msi.read(path)[0])[0]) for path in sys.argv[2:]))
EOF
)" && status=0 || status=$?
expect "string pools in code pages 65001 and 0 decode as UTF-8 and cp1252" 0 "'Pilote \\xe9' 'Pilote \\xe9'"
case_of db-long '"pool_extra": "00000100"' "files: the string pool holds a string over 64 KiB" "" "a string over 64 KiB fails"
case_of db-cp1252 '"pool_extra": "01000100", "data_extra": "81"' "is not cp1252" "" "a string that is not in its code page fails"
case_of db-data '"data_extra": "00"' "files: the string pool and its data disagree on their length" "" "string data past the pool fails"
case_of db-columns '"stream_extra": {"_Columns": "00"}' "files: the _Columns table's size is not a whole number of rows" "" "a _Columns table of part rows fails"
case_of db-rows '"stream_extra": {"File": "00"}' "files: the File table's size is not a whole number of rows" "" "a table of part rows fails"
case_of db-ref '"stream_extra": {"_Tables": "ffff"}' "files: a table names string 65535, past the pool's end" "" "a reference past the string pool fails"
case_of db-gap '"column_gap": "Media"' "files: the Media table's columns are not numbered 1 to 6" "" "columns not numbered from 1 fail"
case_of db-component '"drop_tables": ["Component"]' "files: the MSI has no Component table" "" "an MSI without a Component table fails"
case_of db-loop '"directories": [["TARGETDIR", null, "SourceDir"], ["INSTALLDIR", "NOPE", "x"]]' \
  "files: the Directory table's parents of NOPE loop or are missing" "" "a directory whose parent is missing fails"
case_of db-cycle '"directories": [["TARGETDIR", null, "SourceDir"], ["INSTALLDIR", "LOOP", "x"], ["LOOP", "INSTALLDIR", "y"]]' \
  "files: the Directory table's parents of INSTALLDIR loop or are missing" "" "two directories that name each other as parents fail"
case_of db-root '"directories": [["TARGETDIR", null, "SourceDir"], ["OTHER", null, "x"], ["INSTALLDIR", "OTHER", "y"]]' \
  "files: directory OTHER has no parent and is not TARGETDIR" "" "a directory under a root other than TARGETDIR fails"
case_of db-nodir "" "files: file rsodbc.dll is in no directory under TARGETDIR" "$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll" | sed 's/INSTALLDIR/MISSING/')" \
  "a file in a directory the table does not have fails"
case_of db-sequence '"last_sequence": 1' "files: crypto.dll is in no cabinet embedded in the MSI" "" "a file past the last cabinet's sequence fails"
case_of db-external '"cabinet": "Data1.cab"' "files: rsodbc.dll is in no cabinet embedded in the MSI" "" "a cabinet outside the MSI fails"
case_of db-nostream '"cabinet": "#Other.cab"' "files: the MSI has no stream for its cabinet #Other.cab" "" "a cabinet stream the MSI does not have fails"
case_of db-twice '"media_twice": true' "files: two cabinets hold rsodbc.dll" "" "two Media rows naming one cabinet fail"
# The same two rows with the total bound just over one cabinet: the second is refused by the
# bound the first one left, before the names are compared; just under it, the first is refused.
db_total="$(bounded python3 -I -B -c 'import importlib.util, sys
spec = importlib.util.spec_from_file_location("msi", sys.argv[1]); msi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(msi)
print(sum(f[4] for f in msi.read(sys.argv[2])[1]))' "$tools/lib/msi.py" "$tmp/db-twice.msi")"
contents_under() { # contents_under <MAX_TOTAL>: msi.contents on db-twice.msi; sets out_text and status
  out_text="$(bounded python3 -I -B - "$tools/lib/msi.py" "$tmp/db-twice.msi" "$1" <<'EOF' 2>&1
import importlib.util, sys
spec = importlib.util.spec_from_file_location("msi", sys.argv[1]); msi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(msi)
db, files = msi.read(sys.argv[2])
msi.MAX_TOTAL = int(sys.argv[3])
try:
    msi.contents(db, files)
except (msi.Failure, msi.cab.Failure) as e:
    sys.exit(str(e))
EOF
)" && status=0 || status=$?
}
contents_under $((db_total + 1))
expect "the total decoded bound covers every cabinet together, naming the cabinet and the total" 1 \
  ", and the bound is 1, what the earlier cabinets left of the $((db_total + 1)) bytes (in cabinet #Data1.cab)"
contents_under $((db_total - 1))
expect "a first cabinet over the total decoded bound is refused against the bound itself" 1 \
  ", and the bound is $((db_total - 1)) (in cabinet #Data1.cab)"
case_of db-nocolumns '"no_columns": ["Media"]' "files: the MSI's _Columns table has no column of Media" "" \
  "a table with no columns fails"
case_of db-nulldir '"directories": [["TARGETDIR", null, "SourceDir"], ["PFiles64", "TARGETDIR", null],
  ["INSTALLDIR", "PFiles64", "x"]]' "files: the MSI's tables are not ones this tool reads: AttributeError" "" \
  "a directory with no DefaultDir fails"
msi db-nocustom '"drop_tables": ["CustomAction"]'
verify db-nocustom; expect "an MSI without a CustomAction table passes" 0 "the notices equal the checked-in copies"
python3 -I "$driver_dir/make_pe.py" "$tmp/x86.dll" '{"machine": 332}'
python3 -I "$driver_dir/make_pe.py" "$tmp/pe32.dll" '{"magic": 267}'
for pe in x86 pe32; do
  case_of "not-$pe" "" "files: crypto.dll is not a PE32+ image for x86-64" \
    "$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll"), $(file_row crypto.dll crypto.dll "@$tmp/$pe.dll"), $(file_row setup rsodbcsetup.dll "@$tmp/setup.bin"), $(file_row ini amazon.redshiftodbc.ini x)" \
    "a set file of another machine or optional header ($pe) fails"
done
cp "$t/policy/windows/msi-files.txt" "$tmp/msi-files.txt"
sed 's/^library  crypto.dll  crypto.dll/library  crypto.dll  con.dll/' "$tmp/msi-files.txt" > "$t/policy/windows/msi-files.txt"
case_of device "" "files: the lock would refuse con.dll's row: file name 'con.dll' is a Windows device name" \
  "$(file_row rsodbc.dll rsodbc.dll "@$tmp/rsodbc.dll"), $(file_row crypto.dll con.dll "@$tmp/crypto.dll"), $(file_row setup rsodbcsetup.dll "@$tmp/setup.bin"), $(file_row ini amazon.redshiftodbc.ini x)" \
  "a set file whose row the lock would refuse fails"
cp "$tmp/msi-files.txt" "$t/policy/windows/msi-files.txt"
cp "$t/policy/windows/product.txt" "$tmp/product.txt"
rm "$t/policy/windows/product.txt"
env_extra=(FAKE_CURL=fail); verify good; env_extra=()
expect "a missing product.txt fails" 1 "policy: policy/windows/product.txt cannot be read"
printf '%s\n' "Amazon%20Redshift%20ODBC%20Driver%20(x64)" "Another" > "$t/policy/windows/product.txt"
env_extra=(FAKE_CURL=fail); verify good; env_extra=()
expect "a product.txt of two lines fails" 1 "policy: policy/windows/product.txt holds one line, the ProductName"
cp "$tmp/product.txt" "$t/policy/windows/product.txt"
mkdir -p "$tmp/used"; : > "$tmp/used/x"
out_text="$(python3 -I "$tools/lib/msi.py" check "$tmp/good.msi" "$t/policy" 2.3.0.0 "$msi_url" "$msi_sha" "$tmp/used" 2>&1)" && status=0 || status=$?
expect "check writes only into an empty directory" 1 "files: $tmp/used is not empty"
out_text="$(python3 -I "$tools/lib/msi.py" check "$tmp/good.msi" "$t/policy" 2.3.0.0 "$msi_url" "$msi_sha" "$tmp/no-such-dir" 2>&1)" && status=0 || status=$?
expect "check into a missing directory fails with its reason" 1 "files: $tmp/no-such-dir: No such file or directory"
out_text="$(python3 -I "$tools/lib/msi.py" extract "$tmp/good.msi" "$tmp/no-such-dir" "$dir/rsodbc.dll=rsodbc.dll" 2>&1)" && status=0 || status=$?
expect "extract into a missing directory fails with its reason" 1 "files: $tmp/no-such-dir/rsodbc.dll: No such file or directory"

# Verification's rules before any download, each on its own.
preflight() { # preflight <name> <needle> <platform> <version> <tree> <header> <bundle> [env...]
  rm -f "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" FAKE_CURL=fail "${@:8}" \
    "$BASH" "$tools/lib/verify_windows.sh" "$3" "$4" "$5" "$6" "$7" 2>&1)" && status=0 || status=$?
  if [[ -e "$FAKE_CURL_LOG" ]]; then fail "$1 (it fetched)"; else expect "$1" 1 "$2"; fi
}
preflight "a platform that is not Windows fails" "verify-release: linux_amd64: not a Windows platform" linux_amd64 2.3.0.0 "$t" "$header" "$pki/bundle.pem"
preflight "a version without a release-tag rule fails" "notices: there is no release-tag rule for 2.3.0.1" windows_amd64 2.3.0.1 "$t" "$header" "$pki/bundle.pem"
preflight "a missing interface header fails" "entry-points: the interface header $tmp/none.hpp is missing" windows_amd64 2.3.0.0 "$t" "$tmp/none.hpp" "$pki/bundle.pem"
preflight "a missing trust store fails" "trust-store: no system CA bundle at $tmp/none.pem" windows_amd64 2.3.0.0 "$t" "$header" "$tmp/none.pem"
# A header that exists but cannot be read; root reads it anyway, so then the case skips.
cp "$header" "$tmp/unreadable.hpp"; chmod 000 "$tmp/unreadable.hpp"
if [[ -r "$tmp/unreadable.hpp" ]]; then
  n=$((n+1)); echo "ok $n - an unreadable interface header fails # SKIP this user reads a mode-000 file (root)"
else
  preflight "an unreadable interface header fails" \
    "entry-points: the interface header $tmp/unreadable.hpp is missing or unreadable" windows_amd64 2.3.0.0 "$t" \
    "$tmp/unreadable.hpp" "$pki/bundle.pem"
fi
chmod 600 "$tmp/unreadable.hpp"
mkdir -p "$tmp/old-openssl"
printf '#!/usr/bin/env bash\n[[ "$1" != version ]] || { echo "OpenSSL 1.0.2k-fips  26 Jan 2017"; exit 0; }\nexec %s "$@"\n' \
  "$(command -v openssl)" > "$tmp/old-openssl/openssl"; chmod +x "$tmp/old-openssl/openssl"
preflight "an OpenSSL older than 1.1.0 fails before any download" "openssl: verification needs OpenSSL 1.1.0 or later" \
  windows_amd64 2.3.0.0 "$t" "$header" "$pki/bundle.pem" PATH="$tmp/old-openssl:$tmp/bin:$PATH"
# A PATH with dirname, which the script runs first, and python3 or nothing more.
mkdir -p "$tmp/python-only" "$tmp/dirname-only"
ln -sf "$(command -v python3)" "$tmp/python-only/python3"
ln -sf "$(command -v dirname)" "$tmp/python-only/dirname"; ln -sf "$(command -v dirname)" "$tmp/dirname-only/dirname"
preflight "a missing curl fails" "tools: curl is required and was not found on PATH" windows_amd64 2.3.0.0 "$t" "$header" "$pki/bundle.pem" PATH="$tmp/python-only"
preflight "a missing python3 fails" "tools: python3 is required and was not found on PATH" windows_amd64 2.3.0.0 "$t" "$header" "$pki/bundle.pem" PATH="$tmp/dirname-only"
bad_tree="$(test_tree winmsi-badpin)"; cp -R "$t/policy/windows" "$bad_tree/policy/"; printf 'not a certificate\n' > "$bad_tree/signing/linux/$pinned_name"
preflight "a pinned intermediate that cannot be read fails" "certificate: the pinned intermediate:" windows_amd64 2.3.0.0 "$bad_tree" "$header" "$pki/bundle.pem"
out_text="$(bash "$tools/lib/verify_windows.sh" 2>&1)" && status=0 || status=$?
expect "verify_windows.sh without its arguments is a usage error" 2 "usage: verify_windows.sh"
# The retained copy must be the bytes the recorded lock names.
mkdir -p "$tmp/bad-mirror"; cp "$tmp/product.msi" "$tmp/bad-mirror/$msi_sha"
rm -f "$FAKE_CURL_LOG"
out_text="$(env RS_DRIVER_MIRROR="$tmp/bad-mirror" RS_DRIVER_LOCK="$tmp/win.lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
  bash "$t/tools/lib/verify_windows.sh" windows_amd64 2.3.0.0 "$t" "$header" "$pki/bundle.pem" 2>&1)" && status=0 || status=$?
expect "a retained MSI that is not the recorded lock's fails" 1 "mirror: the retained $tmp/bad-mirror/$msi_sha has SHA-256 $(sha "$tmp/product.msi")"
out_text="$(bash "$tools/verify-release.sh" latest windows_amd64 2>&1)" && status=0 || status=$?
expect "verify-release.sh sends windows_amd64 to the Windows verification" 1 "verify-release: windows_amd64: version: check-lock: 'latest'"

# msi.py record and compare: an MSI's product, actions and files, and an administrative image
# checked against it.
msi ca-record '"custom_action": "AdminExecuteSequence"'
out_text="$(python3 -I "$tools/lib/msi.py" record "$tmp/ca-record.msi" 2>&1)" && status=0 || status=$?
if [[ "$status" == 0 && "$out_text" == *"ProductVersion 2.3.0.0"* && "$out_text" == *"AdminExecuteSequence 900 SetTarget CUSTOM ACTION"* &&
  "$out_text" == *"AdminExecuteSequence 900 SetTarget CUSTOM ACTION"$'\n'"AdminExecuteSequence 3900 InstallAdminPackage"* &&
  "$out_text" == *"leave  crypto.dll  crypto.dll  INSTALLDIR  # $dir/crypto.dll 518 $(sha "$tmp/crypto.dll")"* ]]; then
  pass "record prints the product, each administrative action in sequence order marking a custom one, and each file as a policy row"
else fail "record prints the product, each administrative action in sequence order marking a custom one, and each file as a policy row (exit $status; $out_text)"; fi
image="$tmp/image/PFiles64/Amazon Redshift ODBC Driver (x64)"
mkdir -p "$image"
python3 -I "$tools/lib/msi.py" extract "$tmp/good.msi" "$image" "$dir/rsodbc.dll=rsodbc.dll" "$dir/crypto.dll=crypto.dll" \
  "$dir/rsodbcsetup.dll=rsodbcsetup.dll" "$dir/amazon.redshiftodbc.ini=amazon.redshiftodbc.ini"
out_text="$(python3 -I "$tools/lib/msi.py" compare "$tmp/good.msi" "$tmp/image" 2>&1)" && status=0 || status=$?
expect "compare accepts an administrative image with every file's bytes" 0 "$(sha "$tmp/setup.bin")  $dir/rsodbcsetup.dll"
printf 'x=2' > "$image/amazon.redshiftodbc.ini"
out_text="$(python3 -I "$tools/lib/msi.py" compare "$tmp/good.msi" "$tmp/image" 2>&1)" && status=0 || status=$?
expect "compare refuses a file that differs" 1 "files: $dir/amazon.redshiftodbc.ini in the administrative image differs"
rm "$image/amazon.redshiftodbc.ini"
out_text="$(python3 -I "$tools/lib/msi.py" compare "$tmp/good.msi" "$tmp/image" 2>&1)" && status=0 || status=$?
expect "compare refuses a missing file" 1 "files: the administrative image has no $dir/amazon.redshiftodbc.ini"
out_text="$(python3 -I "$tools/lib/msi.py" extract "$tmp/good.msi" "$tmp" "./nowhere/x.dll=x.dll" 2>&1)" && status=0 || status=$?
expect "extract refuses a path the MSI has no file at" 1 "files: the MSI has no file at ./nowhere/x.dll"
mkdir -p "$tmp/ex/inner"
for name in ../escaped.dll "$tmp/ex/absolute.dll"; do
  out_text="$(python3 -I "$tools/lib/msi.py" extract "$tmp/good.msi" "$tmp/ex/inner" "$dir/rsodbc.dll=$name" 2>&1)" &&
    status=0 || status=$?
  expect "extract refuses the name $name, which is not a file name" 1 "or '$name' is not a file name"
done
[[ ! -e "$tmp/ex/escaped.dll" && ! -e "$tmp/ex/absolute.dll" ]] || fail "a refused extract wrote outside its directory"
# A Directory name of .. puts every file above the image, where copies of them lie, so only the
# containment check refuses it.
msi up '"directories": [["TARGETDIR", null, "SourceDir"], ["INSTALLDIR", "TARGETDIR", ".."]]'
mkdir -p "$tmp/up/image"
python3 -I "$tools/lib/msi.py" extract "$tmp/up.msi" "$tmp/up" "./../rsodbc.dll=rsodbc.dll" "./../crypto.dll=crypto.dll" \
  "./../rsodbcsetup.dll=rsodbcsetup.dll" "./../amazon.redshiftodbc.ini=amazon.redshiftodbc.ini"
out_text="$(python3 -I "$tools/lib/msi.py" compare "$tmp/up.msi" "$tmp/up/image" 2>&1)" && status=0 || status=$?
expect "compare refuses a file the MSI places outside the administrative image" 1 \
  "files: ./../rsodbc.dll is outside the administrative image"

# The LZX decoder: every block type, against an encoder written separately, then its refusals.
bounded python3 -I "$driver_dir/make_lzx_cab.py" "$tmp/lzx.cab" "$tmp/lzx.raw" || fail "make_lzx_cab.py writes the cabinet"
out_text="$(bounded python3 -I "$tools/lib/cab.py" list "$tmp/lzx.cab" 2>&1)" && status=0 || status=$?
expect "verbatim, aligned and uncompressed blocks with E8 translation decode to the encoder's input" 0 "$(sha "$tmp/lzx.raw")  lzx.bin"
# The other windows LZX defines, the 2.3.0.0 cabinet's 2^18 (36 slots) among them: each one's
# position slots size the main tree, and in each the encoder's matches reach its last slot, each slot
# with its own base and extra bits.
for bits in 15 17 18 19 20 21; do
  bounded python3 -I "$driver_dir/make_lzx_cab.py" "$tmp/lzx$bits.cab" "$tmp/lzx$bits.raw" "$bits" ||
    fail "make_lzx_cab.py writes the 2^$bits-byte window's cabinet"
  out_text="$(bounded python3 -I "$tools/lib/cab.py" list "$tmp/lzx$bits.cab" 2>&1)" && status=0 || status=$?
  expect "the encoder's blocks in a 2^$bits-byte window decode to its input" 0 \
    "$(sha "$tmp/lzx$bits.raw")  lzx.bin"
done
lzx_case() { # lzx_case <name> <needle> <python that sets data, the cabinet's bytes>
  python3 -I -B - "$tmp/lzx.cab" "$tmp/case.cab" "$driver_dir/make_lzx_cab.py" "$3" <<'EOF'
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("enc", sys.argv[3]); enc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(enc)
data = bytearray(open(sys.argv[1], "rb").read())
def stream(write, size):  # a cabinet whose one folder is the LZX stream write() makes
    w = enc.Writer(); w.bits(0, 1); write(w); w.align()
    return bytearray(enc.cabinet(bytes(w.out), [], size))
exec(sys.argv[4])
open(sys.argv[2], "wb").write(data)
EOF
  out_text="$(bounded python3 -I "$tools/lib/cab.py" list "$tmp/case.cab" 2>&1)" && status=0 || status=$?
  expect "$1" 1 "$2"
}
lzx_case "a window LZX does not define fails" "cab: the LZX window of 2^22 bytes is not one LZX defines" 'data[43] = 22'
lzx_case "a cabinet that continues another fails" "cab: the cabinet continues another, or reserves room (flags 0x1)" 'data[30] = 1'
lzx_case "a cabinet size that is not its stream's fails" "cab: not a version 1.3 cabinet whose size is its stream's" 'data += b"x"'
lzx_case "a file table past the cabinet fails" "cab: the cabinet's folder or file table is malformed" 'data[16:20] = struct.pack("<I", len(data))'
lzx_case "a data block over 32 KiB fails" "cab: folder 0 has a data block over 32 KiB or past the cabinet" \
  'p = struct.unpack_from("<I", data, 36)[0]; data[p + 6:p + 8] = struct.pack("<H", 40000)'
lzx_case "a folder whose blocks decode to more than its files fails" "cab: folder 0 decodes to 100000 bytes, its files hold 99999" \
  'data[44:48] = struct.pack("<I", 99999)'
lzx_case "a file of no folder fails" "cab: a file names a folder the cabinet does not have" 'data[52:54] = struct.pack("<H", 3)'
lzx_case "a cabinet that runs out of data blocks fails" "cab: folder 0's data blocks run past the cabinet" \
  'data[40:42] = struct.pack("<H", 9); data[8:12] = struct.pack("<I", len(data))'
lzx_case "a truncated cabinet header fails" "cab: the cabinet's header is truncated" 'data = data[:20]'
lzx_case "a cabinet of version 1.2 fails" "cab: not a version 1.3 cabinet whose size is its stream's" 'data[24] = 2'
# The last block one byte longer: its bytes are the same, so only the bound refuses it.
lzx_case "a data block that runs past the cabinet fails" "cab: folder 0 has a data block over 32 KiB or past the cabinet" \
  'p = struct.unpack_from("<I", data, 36)[0]
for _ in range(struct.unpack_from("<H", data, 40)[0] - 1): p += 8 + struct.unpack_from("<H", data, p + 4)[0]
data[p + 4:p + 6] = struct.pack("<H", struct.unpack_from("<H", data, p + 4)[0] + 1)'
# cab_total <cabinet> <bound>: cab.files with a 1 MiB file bound and <bound> on the total.
cab_total() {
  out_text="$(bounded python3 -I -B - "$tools/lib/cab.py" "$1" "$2" <<'EOF' 2>&1
import importlib.util, sys
spec = importlib.util.spec_from_file_location("cab", sys.argv[1]); cab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cab)
try:
    cab.files(open(sys.argv[2], "rb").read(), 1 << 20, int(sys.argv[3]))
except cab.Failure as e:
    sys.exit(str(e))
EOF
)" && status=0 || status=$?
}
cab_total "$tmp/lzx.cab" 99999
expect "a cabinet over the total decoded bound fails before it is decoded" 1 \
  "cab: folder 0 decodes to 100000 bytes, its files hold 100000, and the bound is 99999"
# list's own 128 MiB bound, on 4097 empty blocks that would decode to 32 KiB each.
lzx_case "a cabinet over list's 128 MiB bound fails before it is decoded" \
  "cab: folder 0 decodes to 134250496 bytes, its files hold 134250496, and the bound is 134217728" \
  'files = b"".join(struct.pack("<IIHHHH", size, at, 0, 0, 0, 0x20) + b"f%d\0" % i
                  for i, (size, at) in enumerate([(64 << 20, 0), (64 << 20, 64 << 20), (32768, 128 << 20)]))
start = 36 + 8 + len(files); blocks = struct.pack("<IHH", 0, 0, 32768) * 4097
data = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, start + len(blocks), 0, 44, 0, 3, 1, 1, 3, 0, 0, 0)
data += struct.pack("<IHH", start, 4097, 3 | 21 << 8) + files + blocks'
lzx_case "files that do not follow each other in their folder fail" "cab: folder 0's files do not follow each other from its start" \
  'data[48:52] = struct.pack("<I", 1)'
# A second file of the same name, empty and at the folder's end, so only the name is wrong.
lzx_case "a cabinet that holds one name twice fails" "cab: the cabinet holds lzx.bin twice" \
  'data[68:68] = struct.pack("<IIHHHH", 0, 100000, 0, 0x5000, 0, 0x20) + b"lzx.bin\0"
for at in (8, 36): data[at:at + 4] = struct.pack("<I", struct.unpack_from("<I", data, at)[0] + 24)
data[28:30] = struct.pack("<H", 2)'
# Two folders, each one uncompressed block holding one file.
python3 -I -B - "$driver_dir/make_lzx_cab.py" "$tmp/two.cab" <<'EOF'
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("enc", sys.argv[1]); enc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(enc)
def stream(body):
    w = enc.Writer(); w.bits(0, 1); w.bits(3, 3); w.bits(len(body) >> 8, 16); w.bits(len(body) & 255, 8); w.align()
    return bytes(w.out) + struct.pack("<III", 1, 1, 1) + body + b"\0" * (len(body) & 1)
bodies = [b"first folder", b"second folder!"]
files = b"".join(struct.pack("<IIHHHH", len(b), 0, i, 0, 0, 0x20) + b"f%d\0" % i for i, b in enumerate(bodies))
start = 36 + 16 + len(files)
blocks, folders = b"", b""
for b in bodies:
    s = stream(b)
    folders += struct.pack("<IHH", start + len(blocks), 1, 3 | 16 << 8)
    blocks += struct.pack("<IHH", 0, len(s), len(b)) + s
head = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, start + len(blocks), 0, 52, 0, 3, 1, 2, 2, 0, 0, 0)
open(sys.argv[2], "wb").write(head + folders + files + blocks)
EOF
printf 'first folder' > "$tmp/f0"; printf 'second folder!' > "$tmp/f1"
out_text="$(python3 -I "$tools/lib/cab.py" list "$tmp/two.cab" 2>&1)" && status=0 || status=$?
if [[ "$status" == 0 && "$out_text" == "$(sha "$tmp/f0")  f0"$'\n'"$(sha "$tmp/f1")  f1" ]]; then
  pass "a cabinet of two folders decodes each folder's own files"
else fail "a cabinet of two folders decodes each folder's own files (exit $status; $out_text)"; fi
# Its folders decode to 12 and 14 bytes: under a bound of 25, only their sum is over it.
cab_total "$tmp/two.cab" 25
expect "the total decoded bound covers a cabinet's folders together" 1 \
  "cab: folder 1 decodes to 14 bytes (26 with the folders before it), its files hold 14, and the bound is 25"
cab_total "$tmp/two.cab" 26
expect "a cabinet's folders that together reach the total decoded bound pass" 0 ""
lzx_case "an unknown block type fails" "cab: unknown LZX block type 0" 'data = stream(lambda w: w.bits(0, 27), 10)'
lzx_case "an incomplete Huffman tree fails" "cab: an LZX Huffman tree is not complete" \
  'data = stream(lambda w: (w.bits(1, 3), w.bits(10, 24), w.bits(1, 4), [w.bits(0, 4) for _ in range(19)]), 10)'
lzx_case "an over-full Huffman tree fails" "cab: an LZX Huffman tree is over-full" \
  'data = stream(lambda w: (w.bits(1, 3), w.bits(10, 24), [w.bits(1, 4) for _ in range(3)], [w.bits(0, 4) for _ in range(17)]), 10)'
# A verbatim block whose main tree holds the literal a and the match symbols for offset R0 with
# length headers 0 and 7, and whose length tree is empty, or holds only footer 0.
verbatim='
def block(size, symbols, footer=False):
    def write(w):
        w.bits(1, 3); w.bits(size >> 8, 16); w.bits(size & 255, 8)
        main = [0] * 512
        for s in (97, 256, 263): main[s] = 2
        main[0] = 2
        enc.write_lengths(w, main[:256], [0] * 256); enc.write_lengths(w, main[256:], [0] * 256)
        length = [0] * 249
        if footer: length[0] = length[1] = 1
        enc.write_lengths(w, length, [0] * 249)
        codes = enc.codes(main)
        for s in symbols: w.bits(*codes[s])
        if footer: w.bits(0, 1)
    return write
'
lzx_case "a match before the start of the folder fails" "cab: an LZX match reaches before the start of the folder" \
  "$verbatim"'data = stream(block(10, [256]), 10)'
lzx_case "a match past its block fails" "cab: an LZX match runs past its block or frame" "$verbatim"'data = stream(block(3, [97, 97, 256]), 3)'
lzx_case "a length footer from an empty length tree fails" "cab: an LZX symbol is read from an empty Huffman tree" \
  "$verbatim"'data = stream(block(20, [97, 263]), 20)'
lzx_case "input that ends early fails" "cab: the LZX data ends early" \
  "$verbatim"'data = stream(lambda w: block(1000, [97] * 20)(w), 1000)'
lzx_case "an uncompressed block whose repeated offset is 0 fails" \
  "cab: an LZX repeated offset is 0" \
  'data = stream(lambda w: (w.bits(3, 3), w.bits(0, 16), w.bits(4, 8), w.align(),
                          w.out.extend(struct.pack("<III", 0, 1, 1) + b"abcd")), 4)'
lzx_case "an uncompressed block that ends early fails" "cab: an uncompressed LZX block ends early" \
  'data = stream(lambda w: (w.bits(3, 3), w.bits(1000 >> 8, 16), w.bits(1000 & 255, 8)), 1000)'

# The downloader of the Windows build path, against stand-in connections: no network.
out_text="$(python3 -I -B - "$tools/lib/download.py" "$tmp/dl" <<'EOF' 2>&1
import importlib.util, os, socket, sys
os.environ.update(SSL_CERT_URI="file:/planted", OPENSSL_CONF="/planted.cnf")
spec = importlib.util.spec_from_file_location("dl", sys.argv[1]); dl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dl)
print("imported, set:", [n for n in ("SSL_CERT_URI", "OPENSSL_CONF") if n in os.environ])
os.environ.update(SSL_CERT_URI="file:/planted", OPENSSL_CONF="/planted.cnf")
factory, saw = dl.ssl.create_default_context, []
def counting_factory():
    saw.append([n for n in dl.NO_TRUST_VARIABLES if n in os.environ])
    return factory()
dl.ssl.create_default_context = counting_factory
dl.connect("h.example", None, 30)
print("factory saw:", saw)
out = sys.argv[2]
class Response(object):  # length: the Content-Length as http.client reads it
    def __init__(self, status, chunks, length=None, stall=False, drip=False):
        self.status, self.chunks, self.length = status, list(chunks), length
        self.stall, self.drip = stall, drip
    def isclosed(self): return False
    def read1(self, n):
        if self.drip: return b"1"
        if self.stall and not self.chunks: raise socket.timeout("timed out")
        return self.chunks.pop(0) if self.chunks else b""
waits = []
class Socket(object):
    def settimeout(self, seconds): waits.append(seconds)
def opener(response, seen):
    class Connection(object):
        def __init__(self, host, port, timeout):
            seen.append((host, port, timeout)); self.sock = Socket()
        def request(self, method, path, headers): seen.append(path)
        def getresponse(self): return response
        def close(self): pass
    return Connection
def run(name, url, response, max_bytes=10, clock=None):
    seen = []
    del waits[:]
    try:
        size = dl.download(url, out, max_bytes, opener(response, seen), clock or (lambda: 0))
        print("%s: ok %d %r %r" % (name, size, open(out, "rb").read(), seen))
        os.unlink(out)
    except dl.Failure as e:
        print("%s: failed: %s; left %s; seen %r" % (name, e, os.path.exists(out), seen))
run("good", "https://h.example/a/%2e/b.msi?x", Response(200, [b"12345", b"678"]))
run("http", "http://h.example/b.msi", Response(200, [b"x"]))
run("user", "https://u:p@h.example/b.msi", Response(200, [b"x"]))
run("user-only", "https://u@h.example/b.msi", Response(200, [b"x"]))
run("fragment", "https://h.example/b.msi#f", Response(200, [b"x"]))
run("no-host", "https:///b.msi", Response(200, [b"x"]))
run("port", "https://h.example:notaport/b.msi", Response(200, [b"x"]))
run("redirect", "https://h.example/b.msi", Response(301, []))
run("declared", "https://h.example/b.msi", Response(200, [b"x"], length=11))
run("over", "https://h.example/b.msi", Response(200, [b"123456", b"78901"]))
run("short", "https://h.example/b.msi", Response(200, [b"12"], length=8))
run("stall", "https://h.example/b.msi", Response(200, [b"12"], stall=True))
ticks = iter([0, 1, 700])
run("slow", "https://h.example/b.msi", Response(200, [b"1", b"2", b"3"]), clock=lambda: next(ticks))
clock = iter(range(0, 10000, 29))
run("drip", "https://h.example/b.msi", Response(200, [], drip=True), max_bytes=1000,
    clock=lambda: next(clock))
print("drip waits: %d, the last %r" % (len(waits), waits[-1]))
open(out, "wb").write(b"kept")
run("exists", "https://h.example/b.msi", Response(200, [b"x"]))
print("kept:", open(out, "rb").read())
EOF
)" || true
check_line() { if grep -qF -- "$2" <<<"$out_text"; then pass "$1"; else fail "$1 (output: $out_text)"; fi; }
check_line "a body within the bound downloads, by its path as written, with the timeout on the connection" \
  "good: ok 8 b'12345678' [('h.example', None, 30), '/a/%2e/b.msi?x']"
check_line "an http:// URL is refused before any connection" "http: failed: downloading http://h.example/b.msi failed: only an https:// URL"
check_line "a URL whose port is not a number is refused before any connection" \
  "port: failed: downloading https://h.example:notaport/b.msi failed: its port is not a number from 0 to 65535; left False; seen []"
for url in h.example/b.msi#f /b.msi; do
  check_line "https://$url is refused before any connection" \
    "failed: downloading https://$url failed: only an https:// URL with no user or fragment is downloaded; left False; seen []"
done
check_line "a URL with a user and password is refused before any connection, named without them" \
  "user: failed: downloading https://h.example/b.msi failed: only an https:// URL with no user or fragment is downloaded; left False; seen []"
check_line "a URL with a user and no password is refused before any connection" \
  "user-only: failed: downloading https://h.example/b.msi failed: only an https:// URL with no user or fragment is downloaded; left False; seen []"
check_line "a redirect is refused" "redirect: failed: https://h.example/b.msi answered HTTP 301; redirects are not followed; left False"
check_line "a declared length over the bound fails before the body is read" "declared: failed: downloading https://h.example/b.msi failed: its declared 11 bytes are over the 10-byte bound; left False"
check_line "a body over the bound is stopped and removed" "over: failed: downloading https://h.example/b.msi failed: the body passed the 10-byte bound and was stopped; left False"
check_line "a body that ends short of its declared length fails and is removed" \
  "short: failed: downloading https://h.example/b.msi failed: the body ended after 2 of its "\
"declared 8 bytes; left False"
check_line "a stalled body fails at the read timeout and is removed" "stall: failed: downloading https://h.example/b.msi failed: timed out; left False"
check_line "a slow body fails at the deadline" "slow: failed: downloading https://h.example/b.msi failed: it took longer than 600 seconds; left False"
check_line "a file already at the output is refused and kept" "kept: b'kept'"
late="it took longer than 600 seconds; left False"
check_line "a body sent a byte at a time fails at the deadline" \
  "drip: failed: downloading https://h.example/b.msi failed: $late"
check_line "each wait is given at most the time the deadline leaves" "drip waits: 20, the last 20"
check_line \
  "importing download.py removes the variables that would load other OpenSSL configuration" \
  "imported, set: []"
check_line "the context is made without the variables that would add a trusted certificate" \
  "factory saw: [[]]"
# The same through http.client's own response, over a connected socket pair whose other end
# answers and closes, as a server that sends Connection: close does.
out_text="$(bounded python3 -I -B - "$tools/lib/download.py" "$tmp/dl-closed" <<'EOF' 2>&1
import http.client, importlib.util, os, socket, sys, threading
spec = importlib.util.spec_from_file_location("dl", sys.argv[1]); dl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dl)
def run(name, head, body):
    def serve(sock):
        with sock:
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = sock.recv(4096)
                if not chunk:
                    return
                request += chunk
            sock.sendall(b"HTTP/1.1 200 OK\r\n" + head + b"\r\n\r\n" + body)
    def opener(host, port, timeout):
        ours, theirs = socket.socketpair()
        threading.Thread(target=serve, args=(theirs,), daemon=True).start()
        connection = http.client.HTTPConnection(host, port or 443, timeout=timeout)
        connection.sock = ours  # a connection with a socket sends on it, and never connects
        return connection
    try:
        size = dl.download("https://h.example/b.msi", sys.argv[2], 10, opener)
        print("%s: ok %d %r" % (name, size, open(sys.argv[2], "rb").read()))
        os.unlink(sys.argv[2])
    except dl.Failure as e:
        print("%s: failed: %s; left %s" % (name, e, os.path.exists(sys.argv[2])))
run("close", b"Content-Length: 5\r\nConnection: close", b"12345")
run("odd-length", b"Content-Length: eight", b"12")
run("spaced", b"Content-Length: 8 ", b"12")
EOF
)" || true
check_line "a complete body from a server that closes the connection downloads" "close: ok 5 b'12345'"
check_line "a Content-Length that is not a number is ignored, and the body read to its end" \
  "odd-length: ok 2 b'12'"
check_line "a Content-Length http.client reads despite its spacing bounds the body: a short one fails" \
  "spaced: failed: downloading https://h.example/b.msi failed: the body ended after 2 of its declared 8 bytes; left False"
out_text="$(python3 -I "$tools/lib/download.py" http://h.example/b.msi "$tmp/dl-cli" 10 2>&1)" && status=0 || status=$?
expect "download.py exits 1 on a refused URL" 1 "only an https:// URL with no user or fragment is downloaded"
out_text="$(python3 -I "$tools/lib/download.py" https://h.example/b.msi 2>&1)" && status=0 || status=$?
expect "download.py without its arguments is a usage error" 2 "usage: download.py <url> <out-file> <max-bytes>"
out_text="$(python3 -I "$tools/lib/download.py" https://h.example/b.msi "$tmp/dl-cli" 1e3 2>&1)" && status=0 || status=$?
expect "download.py with a bound that is not a number is a usage error" 2 "usage: download.py <url> <out-file> <max-bytes>"

out_text="$(python3 -I "$tools/lib/msi.py" policy "$odbc/policy" 2>&1)" && status=0 || status=$?
expect "the checked-in Windows policy files are well formed" 0 ""
# The real MSI, once the lock under test names it. Only its signer check skips once the signer
# expires: the files are still taken from the pinned MSI through cab.py and checked against the lock.
lock="$(bash "$tools/check-lock.sh" "${RS_DRIVER_LOCK:-$odbc/driver.lock}")"
row="$(awk '$1 == "driver" && $2 == "windows_amd64"' <<<"$lock")"
if [[ -z "$row" ]]; then
  echo "ok - the real MSI # SKIP the lock under test has no windows_amd64 row"
else
  read -r _ _ _ _ _ url real_sha _ <<<"$row"
  real_msi="$(release_file "$real_sha" "$url" "$real_sha")" ||
    { fail "the real MSI $url is neither cached nor fetched (or held in RS_DRIVER_MIRROR as $real_sha)"; finish; }
  cp "$real_msi" "$tmp/real.msi"
  mkdir -p "$tmp/real-mirror"; cp "$tmp/real.msi" "$tmp/real-mirror/$real_sha"
  for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
    cp "$odbc/$notice" "$tmp/real-mirror/$real_sha-$notice"
  done
  mkdir "$tmp/real-sig"
  python3 -I "$tools/lib/cfb.py" signature "$tmp/real.msi" "$tmp/real.sig"
  python3 -I "$tools/lib/x509.py" authenticode "$tmp/real.sig" "$tmp/real-sig" >/dev/null
  if ! openssl x509 -checkend 0 -noout -in "$tmp/real-sig/leaf.pem" >/dev/null; then
    echo "ok - the real MSI's verification # SKIP its signer has expired, so the pipeline reuses the recorded lock"
  else
    out_text="$(env RS_DRIVER_MIRROR="$tmp/real-mirror" RS_DRIVER_LOCK="${RS_DRIVER_LOCK:-$odbc/driver.lock}" \
      bash "$tools/verify-release.sh" "$(awk '$1 == "version" { print $2 }' <<<"$lock")" windows_amd64 2>&1)" && status=0 || status=$?
    expect "the real MSI passes" 0 "the notices equal the checked-in copies"
    # Each file row of the lock names a file verification takes from the MSI: its kind, name,
    # source-sha256, MSI and extract path, the sha256 being the renamed bytes'.
    verified="$(awk '$1 == "windows_amd64" && ($2 == "driver" || $2 == "library") {
      print $2, $3, $5, $6, $7, $8 }' <<<"$out_text" | LC_ALL=C sort)"
    locked="$(awk '$2 == "windows_amd64" && ($1 == "driver" || $1 == "library") {
      print $1, $3, $9, $6, $7, $8 }' <<<"$lock" | LC_ALL=C sort)"
    name="the lock's Windows file rows are the files verification takes from the real MSI"
    if [[ -n "$locked" && "$verified" == "$locked" ]]; then pass "$name"
    else fail "$name (verified: $verified; locked: $locked)"; fi
  fi
  # The columns verify-release.sh does not reproduce, from the build path's fetch of the same
  # MSI: each file's renamed sha256, the rename and patch rows, the set hash, and the rename
  # checker on the MSI's files and the renamed ones, which reads kept-names.txt.
  lock_file="${RS_DRIVER_LOCK:-$odbc/driver.lock}"
  out_text="$(env RS_DRIVER_MIRROR="$tmp/real-mirror" RS_DRIVER_LOCK="$lock_file" \
    bash "$tools/fetch-driver.sh" windows_amd64 "$tmp/real-set" 2>&1)" && status=0 || status=$?
  expect "the build path's fetch of the real MSI applies the lock's rename and patch rows" 0 \
    "matches the lock"
  mismatched=""
  while read -r _ _ file want _; do
    embedded="$(awk -v n="$file" '$1 == "rename" && $2 == "windows_amd64" && $3 == n {
      print $4 }' <<<"$lock")"
    [[ -f "$tmp/real-set/${embedded:-$file}" && "$(sha "$tmp/real-set/${embedded:-$file}")" == \
      "$want" ]] ||
      mismatched+=" $file"
  done < <(awk '($1 == "driver" || $1 == "library") && $2 == "windows_amd64"' <<<"$lock")
  set_hash="$(awk '$1 == "set-hash" && $2 == "windows_amd64" { print $3 }' <<<"$lock")"
  name="each embedded file of the real set has its row's sha256, and the set hash is the lock's"
  if [[ -z "$mismatched" && -n "$set_hash" ]] &&
    grep -qxF "#define REDSHIFT_DRIVER_SET_SHA256 \"$set_hash\"" "$tmp/real-set/driver_info.h"; \
      then
    pass "$name"
  else fail "$name (mismatched:$mismatched; set-hash $set_hash)"; fi
  mkdir "$tmp/real-original" "$tmp/real-renamed"
  python3 -I "$tools/lib/msi.py" check "$tmp/real.msi" "$odbc/policy" \
    "$(awk '$1 == "version" { print $2 }' <<<"$lock")" "$url" "$real_sha" "$tmp/real-original" \
      >/dev/null
  cp "$tmp/real-set/"*.dll "$tmp/real-renamed/"
  grep '^windows_amd64' "$lock_file" > "$tmp/real-rows"
  # The checker recomputes the lock's name seed, so it reads the MSI's files and the notices, as the
  # pin did.
  mkdir "$tmp/real-check"; cp "$tmp/real-original/"* "$tmp/real-check/"
  cp "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "$tmp/real-check/"
  out_text="$(python3 -I "$tools/lib/rename_check.py" "$tmp/real-rows" "$tmp/real-check" \
    "$tmp/real-renamed" 2>&1)" && status=0 || status=$?
  expect "the rename checker passes the real set's renamed files and recomputes the lock's name seed" 0 ""
  out_text="$(python3 -I "$tools/lib/rename_check.py" "$tmp/real-rows" "$tmp/real-original" \
    "$tmp/real-renamed" 2>&1)" && status=0 || status=$?
  expect "the rename checker refuses the real set's files without the notices the seed covers" 1 \
    "seed: $tmp/real-original holds no LICENSE, one of the notices the seed covers"
  # The names the pin derives from the MSI's files and the checked-in notices (no tool needed).
  mkdir "$tmp/real-pin"; cp "$tmp/real-original/"* "$tmp/real-pin/"
  cp "$odbc/LICENSE" "$odbc/NOTICE" "$odbc/THIRD_PARTY_LICENSES" "$tmp/real-pin/"
  derived="$(python3 -I "$tools/lib/rename_pin.py" names "$tmp/real-pin" | awk '{ $1 = $1; print }' | LC_ALL=C sort)" ||
    derived=""
  pinned="$(awk '$1 == "windows_amd64" && ($2 == "name-seed" || $2 == "rename") { $1 = $1; print }' "$lock_file" |
    LC_ALL=C sort)"
  name="the lock's name-seed and rename rows are the ones the pin derives from the real MSI's files"
  if [[ -n "$derived" && "$derived" == "$pinned" ]]; then pass "$name"
  else fail "$name (derived: $derived; locked: $pinned)"; fi
  # Each prerequisite's minimum is <major>.<minor>.0.0 of the highest linker version among the
  # files that import it.
  minimums="$(python3 -I - "$tmp/real-original" <<'PYEOF2'
import os, struct, sys
found = {}
for name in os.listdir(sys.argv[1]):
    data = open(os.path.join(sys.argv[1], name), "rb").read()
    optional = struct.unpack_from("<I", data, 0x3C)[0] + 24
    linker = (data[optional + 2], data[optional + 3])
    for dll in (b"VCRUNTIME140.dll", b"VCRUNTIME140_1.dll", b"MSVCP140.dll"):
        if dll + b"\0" in data:
            found[dll.decode()] = max(found.get(dll.decode(), (0, 0)), linker)
print(" ".join("%s %d.%d.0.0" % (d, *v) for d, v in sorted(found.items())))
PYEOF2
)"
  locked_minimums="$(awk '$1 == "prerequisite" && $2 == "windows_amd64" {
    print $3, $4 }' <<<"$lock" |
    LC_ALL=C sort | tr '\n' ' ')"
  name="each prerequisite's minimum is the highest linker version among the files that import it"
  if [[ "$minimums " == "$locked_minimums" ]]; then pass "$name"
  else fail "$name (computed: $minimums; locked: $locked_minimums)"; fi
fi
finish
