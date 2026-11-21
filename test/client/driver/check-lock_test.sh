#!/usr/bin/env bash
# Tool tests for third_party/redshift_odbc/tools/check-lock.sh. Each case writes a lock built from
# the rows below with one change, and checks the exit status and message. Run by
# run-tool-tests.sh, which make test_driver calls.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools="$here/../../../third_party/redshift_odbc/tools"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
check() {  # check <name> <expected-exit> <expected-substring> <lock> [env assignment...]
  local name="$1" want="$2" needle="$3" lock="$4" out status
  shift 4
  n=$((n+1))
  if [[ -n "$lock" ]]; then
    out="$(env "$@" bash "$tools/check-lock.sh" "$lock" 2>&1)" && status=0 || status=$?
  else
    out="$(env "$@" bash "$tools/check-lock.sh" 2>&1)" && status=0 || status=$?
  fi
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then
    echo "ok $n - $name"
  else
    echo "not ok $n - $name (exit $status, wanted $want; output: $out)"; failed=$((failed+1))
  fi
}
mk() {  # mk <name> <line>...; writes the lines as a lock and prints its path
  printf '%s\n' "${@:2}" > "$tmp/$1.lock"; echo "$tmp/$1.lock"
}

h1="$(printf 'a%.0s' {1..64})"; h2="$(printf 'b%.0s' {1..64})"; h3="$(printf 'c%.0s' {1..64})"
base="https://s3.amazonaws.com/redshift-downloads/drivers/odbc"
url="$base/2.3.0.0/AmazonRedshiftODBC-64-bit-2.3.0.0.x86_64.rpm"
path="./opt/amazon/redshiftodbcx64/librsodbc64.so"
src="https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v2.3.0"
F="format 1"; V="version 2.3.0.0"; S="status development"
D="linux_amd64  driver  librsodbc64.so  $h1  $url  $h2  $path"
D2="linux_arm64  driver  librsodbc64.so  $h1  $base/2.3.0.0/AmazonRedshiftODBC-64-bit-2.3.0.0.aarch64.rpm  $h2  $path"
N1="*  notice  LICENSE  $h3  $src/LICENSE"
N2="*  notice  NOTICE  $h3  $src/NOTICE"
N3="*  notice  THIRD_PARTY_LICENSES  $h3  $src/THIRD_PARTY_LICENSES"
drow() { echo "linux_amd64  driver  ${1:-librsodbc64.so}  $h1  ${2:-$url}  $h2  ${3:-$path}"; }

# The accepted forms.
expected="$(printf '%s\n' "format 1" "version 2.3.0.0" "status development" \
  "driver linux_amd64 librsodbc64.so $h1 driver $url $h2 $path" \
  "notice LICENSE $h3 notice_license $src/LICENSE" "notice NOTICE $h3 notice_notice $src/NOTICE" \
  "notice THIRD_PARTY_LICENSES $h3 notice_third_party_licenses $src/THIRD_PARTY_LICENSES")"
lock="$(mk good "# a comment" "$F" "" "$V" "$S   # trailing comment" "$D" "$N1" "$N2" "$N3")"
out="$(bash "$tools/check-lock.sh" "$lock")"; n=$((n+1))
if [[ "$out" == "$expected" ]]; then echo "ok $n - prints the normalised lock"; else
  echo "not ok $n - prints the normalised lock (got: $out)"; failed=$((failed+1)); fi
check "accepts the checked-in lock" 0 "version 2.3.1.0" "$tools/../driver.lock"
# The checked-in lock as a whole: both Linux platforms, both macOS platforms (whose rows name the
# one universal library and differ only in the platform), and notice hashes that match the
# checked-in notices. Either status: a release lock or a development lock. Without the macOS rows
# every real-package macOS case would skip.
checked_in="$(bash "$tools/check-lock.sh" "$tools/../driver.lock")"
assets_ok=1
for want in "driver linux_amd64 librsodbc64.so " "driver linux_arm64 librsodbc64.so " \
  "driver osx_amd64 librsodbc64.dylib " "driver osx_arm64 librsodbc64.dylib "; do
  [[ "$checked_in" == *"$want"* ]] || assets_ok=0
done
mac_amd64="$(awk '$1 == "driver" && $2 == "osx_amd64" { $2 = "osx"; print }' <<<"$checked_in")"
mac_arm64="$(awk '$1 == "driver" && $2 == "osx_arm64" { $2 = "osx"; print }' <<<"$checked_in")"
[[ -n "$mac_amd64" && "$mac_amd64" == "$mac_arm64" ]] || assets_ok=0
for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
  file_sha="$(python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$tools/../$notice")"
  [[ "$checked_in" == *"notice $notice $file_sha "* ]] || assets_ok=0
done
n=$((n+1))
if ((assets_ok)); then echo "ok $n - the checked-in lock has both Linux rows, both equal macOS rows and the checked-in notices' hashes"; else
  echo "not ok $n - the checked-in lock has both Linux rows, both equal macOS rows and the checked-in notices' hashes"; failed=$((failed+1)); fi
check "checks the checked-in lock by default" 0 "version 2.3.1.0" "" -u RS_DRIVER_LOCK
# The needle is a hash only the test lock holds, so the case fails if RS_DRIVER_LOCK is ignored.
check "checks the lock RS_DRIVER_LOCK names" 0 "$h1" "" "RS_DRIVER_LOCK=$lock"
check "refuses an empty RS_DRIVER_LOCK" 1 "RS_DRIVER_LOCK is set but empty" "" "RS_DRIVER_LOCK="
check "refuses a lock that does not exist" 1 "cannot read the lock" "$tmp/missing.lock"
check "accepts both Linux platforms" 0 "driver linux_arm64" "$(mk two "$F" "$V" "$S" "$D" "$D2" "$N1" "$N2" "$N3")"
# Both macOS rows name the one universal library of the one universal package, which one
# verification run checks, so they may differ only in their platform.
murl="$base/2.3.0.0/AmazonRedshiftODBC-64-bit.2.3.0.0.universal.pkg"
mpath="./AmazonRedshiftODBC.pkg/Payload/librsodbc64.dylib"
mrow() { echo "$1  driver  ${2:-librsodbc64.dylib}  ${3:-$h1}  ${4:-$murl}  ${5:-$h2}  ${6:-$mpath}"; }
check "accepts two macOS rows that differ only in their platform" 0 "driver osx_arm64" \
  "$(mk mac "$F" "$V" "$S" "$(mrow osx_amd64)" "$(mrow osx_arm64)" "$N1" "$N2" "$N3")"
check "accepts one macOS row alone" 0 "driver osx_arm64" "$(mk mac1 "$F" "$V" "$S" "$(mrow osx_arm64)" "$N1" "$N2" "$N3")"
mac_differs="differs from the osx_amd64 row on line 4 in more than its platform"
check "refuses macOS rows whose library hashes differ" 1 "$mac_differs" \
  "$(mk mac-sha "$F" "$V" "$S" "$(mrow osx_amd64)" "$(mrow osx_arm64 "" "$h3")" "$N1" "$N2" "$N3")"
check "refuses macOS rows whose package hashes differ" 1 "$mac_differs" \
  "$(mk mac-pkg "$F" "$V" "$S" "$(mrow osx_amd64)" "$(mrow osx_arm64 "" "" "" "$h3")" "$N1" "$N2" "$N3")"
check "refuses macOS rows whose package URLs differ" 1 "$mac_differs" \
  "$(mk mac-url "$F" "$V" "$S" "$(mrow osx_amd64)" "$(mrow osx_arm64 "" "" "${murl%.pkg}-2.pkg")" "$N1" "$N2" "$N3")"
check "refuses macOS rows whose paths in the package differ" 1 "$mac_differs" \
  "$(mk mac-path "$F" "$V" "$S" "$(mrow osx_amd64)" "$(mrow osx_arm64 "" "" "" "" "${mpath%.dylib}2.dylib")" "$N1" "$N2" "$N3")"
check "refuses macOS rows whose file names differ" 1 "$mac_differs" \
  "$(mk mac-name "$F" "$V" "$S" "$(mrow osx_amd64)" "$(mrow osx_arm64 librsodbc.dylib)" "$N1" "$N2" "$N3")"
check "refuses macOS rows that differ, in either order" 1 "differs from the osx_arm64 row on line 4" \
  "$(mk mac-order "$F" "$V" "$S" "$(mrow osx_arm64)" "$(mrow osx_amd64 "" "$h3")" "$N1" "$N2" "$N3")"
check "accepts a release lock at 2.3.0.0" 0 "status release" \
  "$(mk rel "$F" "$V" "status release" "$D" "$N1" "$N2" "$N3")"
check "refuses a notice taken from a path in the package" 1 "notice source must be an https:// URL" \
  "$(mk notice-path "$F" "$V" "$S" "$D" "$N1" "*  notice  NOTICE  $h3  ./usr/share/NOTICE" "$N3")"

# Header and version.
check "refuses a malformed version" 1 "is not four dot-separated numbers" "$(mk v3 "$F" "version 2.3.0" "$S" "$D" "$N1" "$N2" "$N3")"
check "refuses latest" 1 "no aliases such as latest" "$(mk latest "$F" "version latest" "$S" "$D" "$N1" "$N2" "$N3")"
check "refuses a version with a leading zero" 1 "without leading zeros" \
  "$(mk v0 "$F" "version 2.03.0.0" "$S" "$D" "$N1" "$N2" "$N3")"
check "refuses a 2.2.4.0 lock marked release" 1 "below 2.3.0.0 must be status development" \
  "$(mk old "$F" "version 2.2.4.0" "status release" "$(drow librsodbc64.so "$base/2.2.4.0/x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a header line with two values" 1 "status takes one value" \
  "$(mk st2 "$F" "$V" "status development release" "$D" "$N1" "$N2" "$N3")"
check "refuses an unknown status" 1 "status must be development or release" "$(mk st "$F" "$V" "status beta" "$D" "$N1" "$N2" "$N3")"
check "refuses an unknown format" 1 "unknown lock format" "$(mk fmt "format 3" "$V" "$S" "$D" "$N1" "$N2" "$N3")"
check "refuses a second version line" 1 "exactly one format, version and status" \
  "$(mk dup "$F" "$V" "$S" "version 2.3.0.1" "$D" "$N1" "$N2" "$N3")"
check "refuses a lock without a status" 1 "exactly one format, version and status" "$(mk nost "$F" "$V" "$D" "$N1" "$N2" "$N3")"
check "refuses a row before the status line" 1 "must come before every row" "$(mk order "$F" "$V" "$D" "$S" "$N1" "$N2" "$N3")"

# Package URLs.
check "refuses an off-path URL" 1 "is not under $base/2.3.0.0/" \
  "$(mk off "$F" "$V" "$S" "$(drow librsodbc64.so https://example.com/redshift-downloads/drivers/odbc/2.3.0.0/x.rpm)" "$N1" "$N2" "$N3")"
check "refuses a version directory that only starts with the version" 1 "is not under $base/2.3.0.0/" \
  "$(mk vprefix "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.3.0.01/x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a URL for another version" 1 "is not under $base/2.3.0.0/" \
  "$(mk otherv "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.2.4.0/x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a .. segment that resolves to another version" 1 "empty, . or .. segment" \
  "$(mk dotdot "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.3.0.0/../2.2.4.0/x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a . segment" 1 "empty, . or .. segment" \
  "$(mk dot "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.3.0.0/./x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a percent-encoded segment" 1 "no %, ? or #" \
  "$(mk pct "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.3.0.0/%2e%2e/2.2.4.0/x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a query string" 1 "no %, ? or #" "$(mk query "$F" "$V" "$S" "$(drow librsodbc64.so "$url?x=1")" "$N1" "$N2" "$N3")"
check "refuses a fragment" 1 "no %, ? or #" "$(mk frag "$F" "$V" "$S" "$(drow librsodbc64.so "$url#x")" "$N1" "$N2" "$N3")"
check "refuses a //" 1 "empty, . or .. segment" \
  "$(mk slashes "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.3.0.0//x.rpm")" "$N1" "$N2" "$N3")"
check "refuses a URL that ends with /" 1 "empty, . or .. segment" \
  "$(mk trailing "$F" "$V" "$S" "$(drow librsodbc64.so "$base/2.3.0.0/")" "$N1" "$N2" "$N3")"
check "refuses plain http" 1 "is not under" \
  "$(mk http "$F" "$V" "$S" "$(drow librsodbc64.so "http://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0/x.rpm")" "$N1" "$N2" "$N3")"

# Hashes and platforms.
check "refuses a short hash" 1 "sha256 must be 64 lower-case hex digits" \
  "$(mk short "$F" "$V" "$S" "linux_amd64 driver librsodbc64.so ${h1:1} $url $h2 $path" "$N1" "$N2" "$N3")"
check "refuses an upper-case hash" 1 "package sha256 must be 64 lower-case hex digits" \
  "$(mk upper "$F" "$V" "$S" "linux_amd64 driver librsodbc64.so $h1 $url $(printf 'B%.0s' {1..64}) $path" "$N1" "$N2" "$N3")"
check "refuses an unknown platform" 1 "unknown platform 'windows_arm64'" \
  "$(mk win "$F" "$V" "$S" "windows_arm64 driver rsodbc.dll $h1 $url $h2 $path" "$N1" "$N2" "$N3")"
check "refuses a notice row for one platform" 1 "its platform is *" \
  "$(mk np "$F" "$V" "$S" "$D" "linux_amd64 notice LICENSE $h3 $src/LICENSE" "$N2" "$N3")"
check "refuses a driver row with a missing field" 1 "a driver row is" \
  "$(mk short-row "$F" "$V" "$S" "linux_amd64 driver librsodbc64.so $h1 $url $h2" "$N1" "$N2" "$N3")"
check "refuses an unknown row" 1 "unknown row" "$(mk unknown "$F" "$V" "$S" "$D" "linux_amd64 library x" "$N1" "$N2" "$N3")"
check "refuses a lock with no driver row" 1 "has no driver row" "$(mk nodriver "$F" "$V" "$S" "$N1" "$N2" "$N3")"
check "refuses a lock without THIRD_PARTY_LICENSES" 1 "no notice row for THIRD_PARTY_LICENSES" \
  "$(mk nonotice "$F" "$V" "$S" "$D" "$N1" "$N2")"
check "refuses a CRLF line end" 1 "byte 0x0d is not allowed" "$(mk crlf "$F" "$V" "$S"$'\r' "$D" "$N1" "$N2" "$N3")"
utf8="$N1  "$'\xc3\xa9'
check "refuses a non-ASCII byte" 1 "byte 0xc3 is not allowed" "$(mk utf8 "$F" "$V" "$S" "$D" "$utf8" "$N2" "$N3")"

# Adversarial file names and paths in the package.
for bad in 'a/b' '.hidden' '-dash' 'a"b' 'a\b' "$(printf 'x%.0s' {1..65})"; do
  check "refuses the file name $bad" 1 "must be 1 to 64 characters" \
    "$(mk name "$F" "$V" "$S" "$(drow "$bad")" "$N1" "$N2" "$N3")"
done
check "refuses a reserved name for the driver too" 1 "is reserved for what the build writes" \
  "$(mk reserved-driver "$F" "$V" "$S" "$(drow packages)" "$N1" "$N2" "$N3")"
for reserved in driver_info.h packages DRIVER_INFO.H; do
  check "refuses the reserved file name $reserved" 1 "is reserved for what the build writes" \
    "$(mk reserved "$F" "$V" "$S" "$D" "$N1" "$N2" "$N3" "*  notice  $reserved  $h3  $src/x")"
done
check "refuses a file name with a space" 1 "a driver row is" \
  "$(mk space "$F" "$V" "$S" "linux_amd64 driver lib rsodbc64.so $h1 $url $h2 $path" "$N1" "$N2" "$N3")"
check "accepts a 64-character file name" 0 "driver linux_amd64" \
  "$(mk long-ok "$F" "$V" "$S" "$(drow "$(printf 'x%.0s' {1..64})")" "$N1" "$N2" "$N3")"
for bad in /opt/x.so ./opt/../x.so .//x.so ./opt/-x.so ./ ./opt/./x.so; do
  check "refuses the path in the package $bad" 1 "path in the package must" \
    "$(mk p "$F" "$V" "$S" "$(drow librsodbc64.so "$url" "$bad")" "$N1" "$N2" "$N3")"
done

# Unique rows, case and symbol stems.
check "refuses two driver rows for one platform" 1 "second driver row for linux_amd64 (first on line 4)" \
  "$(mk twice "$F" "$V" "$S" "$D" "$D" "$N1" "$N2" "$N3")"
check "refuses two notice rows with one name" 1 "repeats or differs only in case" \
  "$(mk n2 "$F" "$V" "$S" "$D" "$N1" "$N1" "$N2" "$N3")"
check "refuses a driver named like a notice, ignoring case" 1 \
  "notice LICENSE has the name of a file of the set" \
  "$(mk nd "$F" "$V" "$S" "$(drow license)" "$N1" "$N2" "$N3")"
check "refuses a notice that is not one of the three, such as a driver configuration file" 1 \
  "notice 'amazon.redshiftodbc.ini' is not one of LICENSE, NOTICE, THIRD_PARTY_LICENSES" \
  "$(mk extra "$F" "$V" "$S" "$D" "$N1" "$N2" "$N3" "*  notice  amazon.redshiftodbc.ini  $h3  $src/x")"
check "refuses a notice that differs from one of the three only in case" 1 "notice 'license' is not one of" \
  "$(mk casefold "$F" "$V" "$S" "$D" "$N1" "*  notice  license  $h3  $src/l" "$N2" "$N3")"

# Notice sources.
check "refuses a notice source with a space" 1 "a notice row is" \
  "$(mk ns "$F" "$V" "$S" "$D" "*  notice  LICENSE  $h3  $src/LI CENSE" "$N2" "$N3")"
check "refuses a notice source with a .. segment" 1 "notice source URL may hold only" \
  "$(mk nsdd "$F" "$V" "$S" "$D" "*  notice  LICENSE  $h3  $src/../LICENSE" "$N2" "$N3")"
check "refuses a notice source path with a .. segment" 1 "notice source must be an https:// URL" \
  "$(mk nspath "$F" "$V" "$S" "$D" "$N1" "*  notice  NOTICE  $h3  ./../NOTICE" "$N3")"
check "refuses a notice row with a short hash" 1 "sha256 must be 64 lower-case hex digits" \
  "$(mk nshash "$F" "$V" "$S" "$D" "$N1" "*  notice  NOTICE  ${h3:1}  $src/NOTICE" "$N3")"
check "refuses a notice source that is not https" 1 "notice source must be an https:// URL" \
  "$(mk nshttp "$F" "$V" "$S" "$D" "*  notice  LICENSE  $h3  http://example.com/LICENSE" "$N2" "$N3")"

# Format 2: the Windows set, several files from one MSI, with its prerequisites and the rename's
# rows. wrow <kind> <name> [<sha256> [<source-sha256> [<extract path>]]] is a file row.
msi="$base/2.3.0.0/AmazonRedshiftODBC64-2.3.0.0.msi"
dir="./PFiles64/Amazon%20Redshift%20ODBC%20Driver%20(x64)"
wrow() { echo "windows_amd64  $1  $2  ${3:-$h1}  ${4:-${3:-$h1}}  $msi  $h2  ${5:-$dir/$2}"; }
W=("$(wrow driver rsodbc.dll)" "$(wrow library crypto.dll)" "$(wrow library krb5_64.dll "$h3" "$h1")"
  "windows_amd64  prerequisite  VCRUNTIME140.dll  14.29.30139.0"
  "windows_amd64  rename  krb5_64.dll  abcdefg.dll"
  "windows_amd64  patch  krb5_64.dll  1024  6b726235  61626364"
  "windows_amd64  name-seed  $h2" "windows_amd64  set-hash  $h3")
# A renamed lock's name seed and set hash.
SEEDS=("${W[@]:6}")
F2="format 2"
w2() { mk "$1" "$F2" "$V" "$S" "${@:2}" "$N1" "$N2" "$N3"; }
check "accepts a Windows lock with each new row kind, beside the Linux and macOS rows" 0 \
  "patch windows_amd64 krb5_64.dll 1024 6b726235 61626364" "$(w2 win2 "$D" "$(mrow osx_amd64)" "$(mrow osx_arm64)" "${W[@]}")"
out="$(bash "$tools/check-lock.sh" "$tmp/win2.lock" 2>&1)" || true; n=$((n+1))
want_lines=("format 2" "driver linux_amd64 librsodbc64.so $h1 driver $url $h2 $path"
  "driver windows_amd64 rsodbc.dll $h1 driver $msi $h2 $dir/rsodbc.dll $h1"
  "library windows_amd64 crypto.dll $h1 library_crypto_dll $msi $h2 $dir/crypto.dll $h1"
  "library windows_amd64 krb5_64.dll $h3 library_krb5_64_dll $msi $h2 $dir/krb5_64.dll $h1"
  "prerequisite windows_amd64 VCRUNTIME140.dll 14.29.30139.0"
  "rename windows_amd64 krb5_64.dll abcdefg.dll" "name-seed windows_amd64 $h2"
  "set-hash windows_amd64 $h3")
missing=""
for l in "${want_lines[@]}"; do grep -qxF -- "$l" <<<"$out" || missing+=" [$l]"; done
if [[ -z "$missing" ]]; then echo "ok $n - prints each Windows row in its normalised form"; else
  echo "not ok $n - prints each Windows row in its normalised form (missing:$missing; got: $out)"; failed=$((failed+1)); fi
check "keeps a format 1 lock's output unchanged" 0 "format 1" "$lock"
check "refuses Windows rows in a format 1 lock" 1 "windows_amd64 rows need lock format 2" \
  "$(mk w1 "$F" "$V" "$S" "$D" "${W[0]}" "$N1" "$N2" "$N3")"
check "accepts a lone 0 version field and a wshelp64 replacement of 8 characters" 0 "prerequisite windows_amd64 MSVCP140.dll 14.0.0.0" \
  "$(w2 zero "${W[0]}" "$(wrow library wshelp64.dll)" "windows_amd64  rename  wshelp64.dll  abcdefgh.dll" \
     "windows_amd64  prerequisite  MSVCP140.dll  14.0.0.0" "${SEEDS[@]}")"
check "accepts a 16-character generated name" 0 "rename windows_amd64 crypto.dll abcdefghijk23456.dll" \
  "$(w2 sixteen "${W[0]}" "${W[1]}" "windows_amd64  rename  crypto.dll  abcdefghijk23456.dll" \
     "${SEEDS[@]}")"
for kind in driver library prerequisite rename patch; do
  check "refuses a $kind row with a missing field" 1 "a $kind row is: windows_amd64 $kind" \
    "$(w2 "short-$kind" "${W[@]}" "windows_amd64  $kind  x.dll")"
done
check "refuses a Windows row kind for another platform" 1 "a library row is for windows_amd64" \
  "$(w2 lib-linux "$D" "linux_amd64  library  x.so  $h1  $h1  $url  $h2  $path")"
check "refuses library rows without a driver row" 1 "library rows need a windows_amd64 driver row" \
  "$(w2 lib-alone "$D" "${W[1]}")"
check "refuses a file row from another MSI" 1 "names another MSI than rsodbc.dll on line 4" \
  "$(w2 two-msi "${W[0]}" "windows_amd64  library  ssl.dll  $h1  $h1  ${msi%.msi}-2.msi  $h2  $dir/ssl.dll")"
check "refuses an MSI URL off the version's path" 1 "is not under $base/2.3.0.0/" \
  "$(w2 msi-off "windows_amd64  driver  rsodbc.dll  $h1  $h1  $base/2.2.4.0/x.msi  $h2  $dir/rsodbc.dll")"
check "refuses a library named like a notice" 1 "notice LICENSE has the name of a file of the set" \
  "$(w2 lib-notice "${W[0]}" "$(wrow library license)")"
check "refuses an upper-case MSI sha256" 1 "msi sha256 must be 64 lower-case hex digits" \
  "$(w2 msi-upper "${W[0]}" \
    "windows_amd64  library  ssl.dll  $h1  $h1  $msi  $(tr a-f A-F <<<"$h2")  $dir/ssl.dll")"
check "refuses a 63-digit MSI sha256" 1 "msi sha256 must be 64 lower-case hex digits" \
  "$(w2 msi-short "${W[0]}" \
    "windows_amd64  library  ssl.dll  $h1  $h1  $msi  ${h2:1}  $dir/ssl.dll")"
check "refuses a file row with the same MSI URL and another MSI sha256" 1 \
  "names another MSI than rsodbc.dll on line 4" \
  "$(w2 msi-other-sha "${W[0]}" \
    "windows_amd64  library  ssl.dll  $h1  $h1  $msi  $h3  $dir/ssl.dll")"
check "accepts two adjacent patches" 0 "patch windows_amd64 krb5_64.dll 1028 aaaa bbbb" \
  "$(w2 adjacent "${W[@]}" "windows_amd64  patch  krb5_64.dll  1028  aaaa  bbbb")"
check "refuses an unknown format 2 row" 1 "unknown row; a row is <platform> driver ..., windows_amd64" \
  "$(w2 unknown2 "${W[0]}" "windows_amd64  config  x.ini")"

# Canonical spellings: one way to write each field.
for pair in "upper-sha $(printf 'A%.0s' {1..64})" "sha-62 ${h1:2}"; do
  read -r what value <<<"$pair"
  check "refuses a Windows sha256 that is $what" 1 "sha256 must be 64 lower-case hex digits" \
    "$(w2 "$what" "$(wrow driver rsodbc.dll "$value")")"
done
check "refuses an upper-case source sha256" 1 "source sha256 must be 64 lower-case hex digits" \
  "$(w2 upper-src "windows_amd64  driver  rsodbc.dll  $h1  $(printf 'A%.0s' {1..64})  $msi  $h2  $dir/rsodbc.dll")"
for v in 14.29.30139 14.029.30139.0 14.00.0.0 14.29.65536.0 14.29.x.0; do
  check "refuses the minimum file version $v" 1 "minimum file version '$v' must be four dot-separated decimals" \
    "$(w2 "ver-$v" "${W[0]}" "windows_amd64  prerequisite  VCRUNTIME140.dll  $v")"
done
for patch in "0x10 aa bb" "010 aa bb" "16 AA bb" "16 aab bb" "16 aa bbcc" "16 aa" "16 - -"; do
  read -r offset old new <<<"$patch"
  case "$offset/$old/$new" in
  0x10/* | 010/*) needle="a patch offset must be decimal without leading zeros" ;;
  */bbcc) needle="original and new bytes must have the same length" ;;
  */) needle="a patch row is" ;;
  *) needle="non-empty lower-case hex of even length" ;;
  esac
  check "refuses the patch '$patch'" 1 "$needle" \
    "$(w2 "patch-$offset-$old" "${W[@]:0:3}" "windows_amd64  patch  krb5_64.dll  $offset  $old  $new")"
done
for bad in abcdef.dll ABCDEFG.dll abcdef0.dll abcdef1.dll abcdef8.dll abcdef9.dll abc_efg.dll abc-efg.dll abcdefg.exe abcdefgh.dll; do
  check "refuses $bad as krb5_64.dll's embedded name" 1 "the embedded name of krb5_64.dll must be 7 characters from a-z2-7" \
    "$(w2 "gen-$bad" "${W[@]:0:4}" "windows_amd64  rename  krb5_64.dll  $bad" "${W[5]}")"
done
check "refuses a 15-character generated name" 1 "the embedded name of crypto.dll must be 16 characters" \
  "$(w2 gen15 "${W[0]}" "${W[1]}" "windows_amd64  rename  crypto.dll  abcdefghijk2345.dll")"

# Windows file names and extract paths.
check "accepts an extract path with a space and parentheses" 0 "$dir/rsodbc.dll" "$(w2 path-ok "${W[0]}")"
for bad in "./a/../rsodbc.dll" "./a//rsodbc.dll" "./-a/rsodbc.dll" "./a./rsodbc.dll" "./a%20/rsodbc.dll" \
  "./a%2e/rsodbc.dll" "./con/rsodbc.dll" "a/rsodbc.dll" "./a/x.dll."; do
  check "refuses the extract path $bad" 1 "extract path must be ./ and segments" \
    "$(w2 "path-${bad//\//_}" "$(wrow driver rsodbc.dll "" "" "$bad")")"
done
for bad in CON con.dll Aux.x.dll LPT9.dll com0 nul.; do
  check "refuses the device name $bad" 1 "file name '$bad' is a Windows device name" "$(w2 "dev-$bad" "${W[0]}" "$(wrow library "$bad")")"
done
check "refuses a trailing dot" 1 "file name 'crypto.dll.' ends with a dot" "$(w2 dot "${W[0]}" "$(wrow library crypto.dll.)")"
check "refuses two file names equal when case is ignored" 1 "file Crypto.DLL repeats or differs only in case from the name on line 5" \
  "$(w2 case "${W[0]}" "${W[1]}" "$(wrow library Crypto.DLL)")"
check "refuses a prerequisite named like a set file" 1 "prerequisite RSODBC.dll repeats or differs only in case" \
  "$(w2 prereq-file "${W[0]}" "windows_amd64  prerequisite  RSODBC.dll  1.0.0.0")"
check "refuses an embedded name that another file has" 1 "embedded name abcdefg.dll repeats or differs only in case" \
  "$(w2 rename-clash "${W[@]:0:3}" "$(wrow library abcdefg.dll)" "${W[4]}" "${W[5]}")"
check "refuses two symbol stems alike" 1 "has the same symbol stem library_a_b_dll" \
  "$(w2 stems "${W[0]}" "$(wrow library a.b.dll)" "$(wrow library a-b.dll)")"
for bad in krbcc64.dll LEASHW64.dll xpprof64.dll amazon.redshiftodbc.ini root.crt rsodbcsetup.dll rsodbcsql.exe; do
  check "refuses a lock that names $bad" 1 "the Windows set does not hold $bad" "$(w2 "deny-$bad" "${W[0]}" "$(wrow library "$bad")")"
done
# Names the set must not hold that fit a generated name's alphabet and length.
for pair in "krb5_64.dll krbcc64.dll" "wshelp64.dll leashw64.dll" "wshelp64.dll xpprof64.dll"; do
  check "refuses a rename of ${pair% *} into ${pair#* }" 1 "the Windows set does not hold ${pair#* }" \
    "$(w2 "deny-rename-${pair#* }" "${W[0]}" "$(wrow library krb5_64.dll)" "$(wrow library wshelp64.dll)" \
       "windows_amd64  rename  ${pair% *}  ${pair#* }")"
done
check "refuses the VC++ runtime as a set file" 1 "vcruntime140.dll is a host prerequisite, never a set file" \
  "$(w2 runtime "${W[0]}" "$(wrow library vcruntime140.dll)")"

# The rename's rows name the set's files and explain every hash difference.
check "refuses a rename of a file that is not in the set" 1 "rename names ssl.dll, which is not a windows_amd64 file" \
  "$(w2 rename-missing "${W[0]}" "windows_amd64  rename  ssl.dll  abcdefghijk23456.dll")"
check "refuses a second rename of one file" 1 "second rename row for crypto.dll" \
  "$(w2 rename-twice "${W[0]}" "${W[1]}" "windows_amd64  rename  crypto.dll  abcdefghijk23456.dll" \
     "windows_amd64  rename  crypto.dll  abcdefghijk23457.dll")"
check "refuses a patch of a file that is not in the set" 1 "patch names ssl.dll, which is not a windows_amd64 file" \
  "$(w2 patch-missing "${W[0]}" "windows_amd64  patch  ssl.dll  0  aa  bb")"
check "refuses two patches that overlap" 1 \
  "this patch of krb5_64.dll overlaps or precedes the one at offset 1024" \
  "$(w2 overlap "${W[@]}" "windows_amd64  patch  krb5_64.dll  1027  aaaa  bbbb")"
check "refuses a patch before the one above it, as the applier does" 1 \
  "this patch of krb5_64.dll overlaps or precedes the one at offset 1024" \
  "$(w2 out-of-order "${W[@]}" "windows_amd64  patch  krb5_64.dll  1020  aaaa  bbbb")"
check "refuses a patched file whose two hashes are equal" 1 "krb5_64.dll has patch rows, so its sha256 must differ" \
  "$(w2 patched-same "${W[0]}" "$(wrow library krb5_64.dll)" "${W[5]}")"
check "refuses an unpatched file whose two hashes differ" 1 "krb5_64.dll has no patch row, so its sha256 must equal" \
  "$(w2 unpatched-diff "${W[@]:0:3}")"

# A renamed lock records its name seed and set hash once each, and only a renamed lock has them.
for i in 0 1; do
  kind="${SEEDS[i]}"; kind="${kind#windows_amd64  }"; kind="${kind%%  *}"
  check "refuses rename rows without the $kind row" 1 \
    "a lock with rename rows records its $kind row" \
    "$(w2 "no-$kind" "${W[@]:0:6}" "${SEEDS[1 - i]}")"
  check "refuses a $kind row without rename rows" 1 "a $kind row needs the rename rows it records" \
    "$(w2 "alone-$kind" "${W[0]}" "${W[1]}" "${SEEDS[i]}")"
  check "refuses a second $kind row" 1 "second $kind row (first on line $((10 + i)))" \
    "$(w2 "twice-$kind" "${W[@]}" "windows_amd64  $kind  $h1")"
  check "refuses a $kind that is not lower-case hex" 1 "a $kind is 64 lower-case hex digits" \
    "$(w2 "upper-$kind" "${W[@]:0:6}" "${SEEDS[1 - i]}" \
       "windows_amd64  $kind  $(printf 'A%.0s' {1..64})")"
  check "refuses a $kind row with a missing field" 1 \
    "a $kind row is: windows_amd64 $kind <sha256>" \
    "$(w2 "short-$kind" "${W[@]:0:6}" "${SEEDS[1 - i]}" "windows_amd64  $kind")"
  check "refuses a $kind row for another platform" 1 "a $kind row is for windows_amd64" \
    "$(w2 "linux-$kind" "$D" "linux_amd64  $kind  $h1")"
done

# A native Windows Python's text stdout turns "\n" into "\r\n", and the readers keep a CR. lock.py
# runs here under a stdout that translates so; its own text write is the control that it does.
lf_only="$(python3 -I - "$tools/lib/lock.py" "$tmp/good.lock" <<'PY'
import io, runpy, sys
raw = io.BytesIO()
sys.stdout = io.TextIOWrapper(raw, newline="\r\n", write_through=True)
sys.stdout.write("control\n")
sys.argv = sys.argv[1:]
try:
    runpy.run_path(sys.argv[0], run_name="__main__")
except SystemExit as e:
    code = e.code
data = raw.getvalue()
sys.__stdout__.write("exit %s control %r rest-cr %d lines %d\n"
                     % (code, data[:9], data[9:].count(b"\r"), data[9:].count(b"\n")))
PY
)"; n=$((n+1))
if [[ "$lf_only" == "exit 0 control b'control\r\n' rest-cr 0 lines 7" ]]; then
  echo "ok $n - writes the normalised lock with LF line ends under a CRLF text stdout"
else
  echo "not ok $n - writes the normalised lock with LF line ends under a CRLF text stdout ($lf_only)"
  failed=$((failed+1))
fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
