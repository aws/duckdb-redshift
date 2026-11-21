#!/usr/bin/env bash
# What run-e2e.sh checks before it starts a case: that the build is an embedded ODBC build, that the
# process has no network (read from its own /proc/net/dev; nothing skips or redirects that), and that the
# system's directories show root as their owner, which `unshare -rn` does not give (the loader would refuse
# them, so every case that loads the driver would fail with a sqllogictest mismatch). It
# runs against a stand-in build directory and needs no build and no network; the cases that need a namespace
# or a networked host say so and are skipped where they cannot run. The Windows branch runs under
# stand-ins for Git's bash (uname, cygpath, python3 for windows_modules.py's Windows commands, the
# unit tester and the shells): its checks before any case, what it hands the shells, and its judging
# of their module lists, trust files and the per-user cache. The real cases run on a Windows host.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/run-e2e.sh"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
n=0; failed=0
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}
skip() { n=$((n+1)); echo "ok $n - # SKIP $1"; }

mkdir -p "$tmp/build/test" "$tmp/build/extension/redshift"
printf '#!/bin/sh\nexit 0\n' > "$tmp/build/test/unittest"
chmod +x "$tmp/build/test/unittest"
: > "$tmp/build/extension/redshift/redshift.duckdb_extension"
embedded='REDSHIFT_CLIENT:STRING=odbc\nREDSHIFT_DRIVER:STRING=embedded\nREDSHIFT_TEST_ENDPOINTS:BOOL=ON\n'
# shellcheck disable=SC2059  # the format is the cache's two lines
printf "$embedded" > "$tmp/build/CMakeCache.txt"

# says <text> <command...>: the command fails and its output holds the text.
says() {
  local text="$1" out; shift
  out="$("$@" 2>&1)" && { echo "it succeeded: $out" >&2; return 1; }
  [[ "$out" == *"$text"* ]] || { echo "no '$text' in: $out" >&2; return 1; }
}
# does_not_say <text> <command...>: whatever the command does, its output lacks the text.
does_not_say() {
  local text="$1" out; shift
  out="$("$@" 2>&1 || true)"
  [[ "$out" != *"$text"* ]] || { echo "found '$text' in: $out" >&2; return 1; }
}

printf 'REDSHIFT_CLIENT:STRING=odbc\nREDSHIFT_DRIVER:STRING=none\n' > "$tmp/build/CMakeCache.txt"
check "a build without the driver is refused" says "is not an embedded ODBC build" bash "$script" "$tmp/build"
printf 'REDSHIFT_CLIENT:STRING=mock\nREDSHIFT_DRIVER:STRING=embedded\n' > "$tmp/build/CMakeCache.txt"
check "a mock build is refused" says "is not an embedded ODBC build" bash "$script" "$tmp/build"
printf 'REDSHIFT_CLIENT:STRING=odbc\nREDSHIFT_DRIVER:STRING=embedded\nREDSHIFT_TEST_ENDPOINTS:BOOL=OFF\n' > "$tmp/build/CMakeCache.txt"
check "an embedded build without the test-endpoint switch is refused" \
  says "run make driver_e2e" bash "$script" "$tmp/build"
# shellcheck disable=SC2059
printf "$embedded" > "$tmp/build/CMakeCache.txt"

check "a stock shell that does not exist is refused" \
  says "is not an executable DuckDB shell" bash "$script" "$tmp/build" "$tmp/no-such-shell"
check "a directory given as the stock shell is refused" \
  says "is not an executable DuckDB shell" bash "$script" "$tmp/build" "$tmp/build"
: > "$tmp/shell-644"
check "a stock shell that is not executable is refused" \
  says "is not an executable DuckDB shell" bash "$script" "$tmp/build" "$tmp/shell-644"
# The Linux branch takes no .exe: a build with only Windows names is refused there.
mkdir -p "$tmp/exe-build/test" "$tmp/exe-build/extension/redshift"
cp "$tmp/build/test/unittest" "$tmp/exe-build/test/unittest.exe"
: > "$tmp/exe-build/extension/redshift/redshift.duckdb_extension"
check "off Windows, a build with only .exe programs is refused" \
  says "has no unittest or loadable extension" bash "$script" "$tmp/exe-build"

# The network check reads this process's own /proc/net/dev, which no variable replaces. On a host with a network
# the script is refused; under `unshare -rn` (no network, but the host's root unmapped) it gets past the network
# check and is refused for the namespace instead, before any case.
if env -u REDSHIFT_ALLOW_NETWORK bash "$here/../odbc/check-no-network.sh" /proc/net/dev >/dev/null 2>&1; then
  skip "this process has no network, so the refusal of one cannot be shown here"
else
  check "a process with a network is refused" \
    says "this process has a network" env -u REDSHIFT_ALLOW_NETWORK bash "$script" "$tmp/build"
  check "REDSHIFT_ALLOW_NETWORK=1 does not skip the network check" \
    says "this process has a network" env REDSHIFT_ALLOW_NETWORK=1 bash "$script" "$tmp/build"
  check "a RUN_E2E_NET_DEV in the environment does not change the file the check reads" \
    says "this process has a network" env RUN_E2E_NET_DEV=/dev/null bash "$script" "$tmp/build"
fi
if [[ "$(id -u)" != 0 ]] && unshare -rn true 2>/dev/null; then
  check "with no network the check passes" \
    does_not_say "this process has a network" env -u REDSHIFT_ALLOW_NETWORK unshare -rn bash "$script" "$tmp/build"
  check "a user namespace that does not map the host's root is refused (unshare -rn)" \
    says "does not map the host's root" env -u REDSHIFT_ALLOW_NETWORK unshare -rn bash "$script" "$tmp/build"
else
  skip "no unprivileged user namespace here (or running as root), so unshare -rn cannot be tried"
fi

# The Windows branch, under Git's bash names: uname, cygpath, python3 (for windows_modules.py's
# Windows commands), the unit tester and the shells stand in, and C:\ is the folder $drive. Each
# Windows program records in $handed what it was handed, which must hold no POSIX path; a flag file
# in $win/flags makes a stand-in misbehave in one way.
win="$tmp/win"
drive="$win/c"
mkdir -p "$win/bin" "$win/uname-only" "$win/flags" "$drive/temp" "$drive/cache" \
  "$win/build/test" "$win/build/extension/redshift/redshift_driver" "$win/stock" \
  "$drive/Windows/System32"
# The DLL run-e2e.sh plants, which a stand-in "loads" only as Windows would: when it is an image.
printf 'MZ a benign image\n' > "$drive/Windows/System32/version.dll"
printf '#define REDSHIFT_DRIVER_SYSTEM_DBGHELP 1\n' \
  > "$win/build/extension/redshift/redshift_driver/driver_info.h"
handed="$win/handed.log"
lock="$win/build/extension/redshift/redshift_driver.lock"
real_python="$(command -v python3)"
h1="$(printf 'a%.0s' {1..64})"; h2="$(printf 'b%.0s' {1..64})"; h3="$(printf 'c%.0s' {1..64})"
rpm="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0"
rpm+="/AmazonRedshiftODBC-64-bit-2.3.0.0.x86_64.rpm"
msi="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0"
msi+="/AmazonRedshiftODBC64-2.3.0.0.msi"
from="./PFiles64/Amazon%20Redshift%20ODBC%20Driver%20(x64)"
notices="https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v2.3.0"
renames=("windows_amd64  rename  rsodbc.dll  abcdefghijk23456.dll"
  "windows_amd64  rename  crypto.dll  bcdefghijk234567.dll"
  "windows_amd64  name-seed  $h2" "windows_amd64  set-hash  $h3")
so="./opt/amazon/redshiftodbcx64/librsodbc64.so"
# write_lock <rename row>...: the build's lock, with a Linux row and the two-file Windows set.
write_lock() {
  printf '%s\n' "format 2" "version 2.3.0.0" "status development" \
    "linux_amd64  driver  librsodbc64.so  $h1  $rpm  $h2  $so" \
    "windows_amd64  driver  rsodbc.dll  $h1  $h1  $msi  $h2  $from/rsodbc.dll" \
    "windows_amd64  library  crypto.dll  $h1  $h1  $msi  $h2  $from/crypto.dll" "$@" \
    "*  notice  LICENSE  $h3  $notices/LICENSE" "*  notice  NOTICE  $h3  $notices/NOTICE" \
    "*  notice  THIRD_PARTY_LICENSES  $h3  $notices/THIRD_PARTY_LICENSES" > "$lock"
}
write_lock "${renames[@]}"
: > "$win/build/extension/redshift/redshift.duckdb_extension"
# shellcheck disable=SC2059
printf "$embedded" > "$win/build/CMakeCache.txt"
printf '#!/bin/sh\necho MINGW64_NT-10.0-20348\n' > "$win/bin/uname"
cp "$win/bin/uname" "$win/uname-only/uname"
# cygpath -u <Windows path> and cygpath -w [-l] <path>: C:\ is $drive, and any other POSIX path is
# under C:\msys64, as Git's own folders are.
cat > "$win/bin/cygpath" <<STUB
#!/usr/bin/env bash
drive='$drive'
STUB
cat >> "$win/bin/cygpath" <<'STUB'
if [[ "$1" == -u ]]; then
  p="${2//\\//}"
  case "$p" in
  C:/msys64/*) p="${p#C:/msys64}" ;;
  C:/*) p="$drive/${p#C:/}" ;;
  esac
  printf '%s\n' "$p"
  exit
fi
[[ "$1" == -w ]] || exit 2
[[ "$2" != -l ]] || shift
p="$2"
case "$p" in
"$drive"/*) p="C:/${p#"$drive"/}" ;;
/*) p="C:/msys64$p" ;;
esac
printf '%s\n' "${p//\//\\}"
STUB
# python3 runs windows_modules.py's folders and modules as Windows would; anything else is Python.
# modules runs the shell on the SQL and lists kernel32 and each file the shell says it loaded, in
# the \\?\ form a module list can give.
cat > "$win/bin/python3" <<STUB
#!/usr/bin/env bash
real='$real_python' flags='$win/flags'
STUB
cat >> "$win/bin/python3" <<'STUB'
if [[ "$1" == -I && "$2" == *windows_modules.py ]]; then
  case "$3" in
  folders)
    [[ ! -e "$flags/folders-fail" && ! ( -e "$flags/case-folders-fail" && "${USERPROFILE:-}" == *profile-* ) ]] ||
      exit 1
    # The per-user cache follows USERPROFILE, as the known folder expands %USERPROFILE%: one
    # C:\cache\<profile's last name> folder per profile, C:\Users\runner's when none is set.
    profile="${USERPROFILE:-C:\\Users\\runner}"
    printf 'C:\\temp\r\nC:\\cache\\%s\\duckdb-redshift\r\n' "${profile##*\\}"
    exit 0 ;;
  modules)
    list="$(cygpath -u "$4")" out="$(cygpath -u "$5")"
    sql="$(cygpath -u "$6")" shell="$(cygpath -u "$7")"
    shift 7
    status=0
    "$shell" "$@" < "$sql" > "$out" 2>&1 || status=$?
    [[ "$status" == 1 ]] || { echo "shell: it exited with status $status" >&2; exit 1; }
    { echo 'C:\Windows\System32\KERNEL32.DLL'
      if [[ -e "$flags/dbghelp-elsewhere" ]]; then echo 'C:\temp\dbghelp.dll'
      elif [[ ! -e "$flags/no-dbghelp" ]]; then echo 'C:\Windows\System32\dbghelp.dll'; fi
      sed -n 's/^loaded //p' "$out" | while IFS= read -r f; do echo "\\\\?\\$(cygpath -w "$f")"; done
    } > "$list"
    exit 0 ;;
  esac
fi
# Windows' Python writes text with CRLF line ends. awk, as BSD sed reads \r as a plain r.
if [[ -e "$flags/crlf" ]]; then
  "$real" "$@" | awk '{ printf "%s\r\n", $0 }'
  exit "${PIPESTATUS[0]}"
fi
exec "$real" "$@"
STUB
# What a Windows program records: its name, arguments and the environment the cases set (the
# stand-ins' text, expanded when they run).
# shellcheck disable=SC2016
record='printf "%s %s USERPROFILE=%s REDSHIFT_DRIVER_DIR=%s TEMP=%s SystemRoot=%s WINDIR=%s" \
  "$(basename "$0")" "$*" "${USERPROFILE:-}" "${REDSHIFT_DRIVER_DIR:-}" "${TEMP:-}" \
  "${SystemRoot:-}" "${WINDIR:-}" >> "$handed"
printf " LEAK=%s\n" "${RUN_E2E_LEAK:-}" >> "$handed"'
# The unit tester: the no-network case writes the set, refused-variable writes nothing.
cat > "$win/build/test/unittest.exe" <<STUB
#!/usr/bin/env bash
handed='$handed' flags='$win/flags'
$record
STUB
cat >> "$win/build/test/unittest.exe" <<'STUB'
if [[ -z "${KRB5_CONFIG:-}" && ! -e "$flags/no-set" || -e "$flags/refused-writes" ]]; then
  mkdir "$(cygpath -u "$REDSHIFT_DRIVER_DIR")/.redshift-driver"
fi
echo "All tests passed (2 assertions in 1 test case)"
STUB
# A shell: the set is written, and loaded, beside the extension: the installed file when the SQL
# installs one (beside the shell itself under the flag beside-host), else the shell. It writes the
# trust file there and fails the connect, and its flags make it misbehave. It refuses a LOAD without
# -unsigned, and, in planted-dlls, a planted location that lacks a name.
cat > "$win/stock/duckdb.exe" <<STUB
#!/usr/bin/env bash
handed='$handed' flags='$win/flags' drive='$drive'
$record
STUB
cat >> "$win/stock/duckdb.exe" <<'STUB'
flag() { [[ -e "$flags/$1" ]]; }
# As on Windows, the per-user cache resolves only where the profile holds AppData\Local.
[[ -d "$(cygpath -u "${USERPROFILE:-}")/AppData/Local" ]] || { echo "no per-user cache"; exit 3; }
sql="$(cat)"
printf '%s\n' "$sql" >> "$handed"
[[ "$sql" != *LOAD* || "$*" == -unsigned ]] || { echo "LOAD of an unsigned extension"; exit 3; }
base="$(cd "$(dirname "$0")" && pwd)"
override=""
# The loader's first base: with REDSHIFT_DRIVER_DIR set, the set is there, and for a folder this
# account cannot write the trust file goes to the per-user cache (ChooseTrustDirectory).
if [[ -n "${REDSHIFT_DRIVER_DIR:-}" ]]; then base="$(cygpath -u "$REDSHIFT_DRIVER_DIR")" override=1; fi
planted=(rsodbc.dll crypto.dll abcdefghijk23456.dll bcdefghijk234567.dll krbcc64.dll leashw64.dll
  xpprof64.dll DbgHelp.dll Secur32.dll)
if [[ -e "$base/DbgHelp.dll" || -e DbgHelp.dll || -e "${PATH%%:*}/DbgHelp.dll" ]]; then
  for dir in "$base" "$PWD" "${PATH%%:*}"; do
    for name in "${planted[@]}"; do [[ -f "$dir/$name" ]] || { echo "no $dir/$name"; exit 3; }; done
  done
fi
if [[ "$sql" =~ extension_directory\ =\ \'([^\']*)\' ]]; then
  installed="$(cygpath -u "${BASH_REMATCH[1]}")/v2.0.0/windows_amd64"
  [[ "$sql" =~ INSTALL\ \'([^\']*)\' ]]
  copy="$(cygpath -u "${BASH_REMATCH[1]}")"
  if ! flag no-install; then mkdir -p "$installed" && cp "$copy" "$installed/"; fi
  if flag twice-install; then
    mkdir -p "${installed/v2.0.0/v1.4.0}" && cp "$copy" "${installed/v2.0.0/v1.4.0}/"
  fi
  if flag beside-copy; then mkdir -p "$(dirname "$copy")/.redshift-driver"; fi
  flag beside-host || base="$installed"
fi
if [[ -z "$override" ]]; then mkdir -p "$base/.redshift-driver/set" "$base/trust"; fi
# A load that adds a file to the read-only tree it was given, dated, with its folder, before the
# run's mark, so only the tree's listing shows it.
if [[ -n "$override" ]] && flag admin-add; then
  : > "$base/.redshift-driver/set/added.dll"
  touch -t 200001010000 "$base/.redshift-driver/set/added.dll" "$base/.redshift-driver/set"
fi
# A run whose trust folder already holds a file is a second process on the first one's folders.
again=""
if [[ -z "$override" && -n "$(ls -A "$base/trust")" ]]; then again=1; fi
# A second process that judged the set invalid sets it aside and writes it again, in the same folder.
if [[ -n "$again" ]] && flag again-set-aside; then
  mv "$base/.redshift-driver/set" "$base/.redshift-driver/.invalid-again"
  mkdir "$base/.redshift-driver/set"
fi
for name in abcdefghijk23456.dll bcdefghijk234567.dll; do
  # Written once, as the loader writes a set: a valid one is used as it is.
  if [[ -z "$override" && ! -e "$base/.redshift-driver/set/$name" ]]; then
    : > "$base/.redshift-driver/set/$name"
  fi
  # A second process that rewrites a file of the set in place, under its own name; dated ahead, so
  # the run's check sees it on a file system with coarse times too.
  if [[ -n "$again" ]] && flag again-rewrite; then
    printf 'rewritten\n' > "$base/.redshift-driver/set/$name"
    touch -t 203001010000 "$base/.redshift-driver/set/$name"
  fi
  # A load that rewrites a file of the read-only tree it was given in place, dated ahead as above.
  if [[ -n "$override" ]] && flag admin-rewrite && [[ -f "$base/.redshift-driver/set/$name" ]]; then
    printf 'rewritten\n' > "$base/.redshift-driver/set/$name"
    touch -t 203001010000 "$base/.redshift-driver/set/$name"
  fi
  if [[ -n "$override" ]] && flag admin-elsewhere; then
    echo "loaded $drive/elsewhere/$name"; continue
  fi
  if flag loaded-elsewhere || { [[ -n "$again" ]] && flag again-elsewhere; }; then
    echo "loaded $drive/elsewhere/$name"
  else echo "loaded $base/.redshift-driver/set/$name"; fi
done
echo "loaded $drive/Windows/System32/version.dll" # the planted copies' source, not a planted file
if flag doubled; then echo "loaded $base/.redshift-driver/set/abcdefghijk23456.dll"; fi
if flag original; then echo "loaded $base/crypto.dll"; fi
# Windows maps an image and refuses a text file, so a planted file is "loaded" only when it is one.
if flag planted && [[ "$(head -c 2 "$base/DbgHelp.dll" 2>/dev/null)" == MZ ]]; then
  echo "loaded $base/DbgHelp.dll"
fi
if flag planted-copy && [[ -e "$base/DbgHelp.dll" ]]; then
  mkdir -p "$drive/elsewhere" && cp "$base/DbgHelp.dll" "$drive/elsewhere/innocent.dll"
  echo "loaded $drive/elsewhere/innocent.dll"
fi
name="$(printf 'pem\n' | sha256sum | cut -d' ' -f1).pem"
if flag wrong-trust; then name="$(printf '0%.0s' {1..64}).pem"; fi
cache="$drive/cache/${USERPROFILE##*\\}/duckdb-redshift/trust"
if [[ -n "$override" ]]; then
  # The read-only tree takes no trust file: it goes to the per-user cache's trust folder.
  mkdir -p "$cache"
  if flag admin-wrong-trust; then name="$(printf '0%.0s' {1..64}).pem"; fi
  if ! flag no-cache-trust; then printf 'pem\n' > "$cache/$name"; fi
  # A copy of the trust file's own bytes, under a name that sorts after it, so only the count of
  # files refuses it.
  if flag two-cache-trust; then printf 'pem\n' > "$cache/other.pem"; fi
elif ! flag no-trust; then
  printf 'pem\n' > "$base/trust/$name"
fi
# A second run under the same profile writes another trust file, which must fail the case.
if flag other-trust && [[ -z "$override" ]] && [[ -e "$base/trust/second.pem" ]]; then
  printf 'pem2\n' > "$base/trust/$(printf 'pem2\n' | sha256sum | cut -d' ' -f1).pem"
fi
if flag other-trust && [[ -z "$override" ]]; then : > "$base/trust/second.pem"; fi
if flag cache; then mkdir -p "$cache" && : > "$cache/new.pem"; fi
if flag cache-newline; then mkdir -p "$cache" && : > "$cache/new.pem"$'\n'kept; fi
if flag refused; then echo "IO Error: the Redshift driver could not be loaded"; exit 1; fi
if flag clean-exit; then exit 0; fi
echo "IO Error: Could not connect to Redshift at 127.0.0.1 (region us-east-1, workgroup" \
  "analytics): the connection attempt failed."
exit 1
STUB
cp "$win/stock/duckdb.exe" "$win/build/duckdb.exe"
chmod +x "$win/bin/"* "$win/uname-only/uname" "$win/build/test/unittest.exe" \
  "$win/build/duckdb.exe" "$win/stock/duckdb.exe"
# windows_run [VAR=value...]: run-e2e.sh under the stand-ins, as Git's bash runs it on Windows,
# with a variable of the caller's that no case may see.
windows_run() {
  env PATH="$win/bin:$PATH" SYSTEMROOT='C:\Windows' WINDIR='C:\Windows' RUN_E2E_LEAK=leaked "$@" \
    bash "$script" "$win/build" "$win/stock/duckdb.exe"
}
# before_any_case <text> [VAR=value...]: the run fails, saying <text>, before any case starts.
before_any_case() {
  local text="$1" out; shift
  out="$(windows_run "$@" 2>&1)" && { echo "it succeeded: $out" >&2; return 1; }
  [[ "$out" == *"$text"* && "$out" != *"run-e2e: no-network"* ]] ||
    { echo "no '$text' before any case in: $out" >&2; return 1; }
}
# flagged <flag> <text>: the run fails, saying <text>, with the stand-ins' <flag> set.
flagged() {
  local status=0
  touch "$win/flags/$1"
  says "$2" windows_run || status=1
  rm -f "$win/flags/$1"
  rm -rf -- "${win:?}/stock/.redshift-driver" "${win:?}/stock/trust"
  return "$status"
}
# flagged_admin <flag> <text>: flagged, for a case that runs only with the tree named.
flagged_admin() {
  local status=0
  touch "$win/flags/$1"
  says "$2" windows_run REDSHIFT_E2E_ADMIN_DIR='C:\admin-tree' || status=1
  rm -f "$win/flags/$1"
  rm -rf -- "${win:?}/stock/.redshift-driver" "${win:?}/stock/trust"
  return "$status"
}
# passes_on_windows [VAR=value...]: the whole run passes, and says it has no no-network proof.
# $want_skip is the one case that may skip, the read-only administrator tree's unless a run names
# one (REDSHIFT_E2E_ADMIN_DIR); nothing else may.
want_skip=read-only-admin-tree
passes_on_windows() {
  local out
  : > "$handed"
  out="$(windows_run "$@" 2>&1)" || { echo "it failed: $out" >&2; return 1; }
  [[ "$out" == *"no no-network proof"* && "$out" == *"run-e2e: loadable-extension-again"* &&
    "$out" == *"run-e2e: passed" ]] || { echo "unexpected: $out" >&2; return 1; }
  if [[ -n "$want_skip" ]]; then
    [[ "$(grep -c '# SKIP' <<<"$out")" == 1 && "$out" == *"run-e2e: $want_skip # SKIP"* ]] ||
      { echo "not only $want_skip skipped: $out" >&2; return 1; }
  else
    [[ "$out" != *SKIP* ]] || { echo "a case skipped: $out" >&2; return 1; }
  fi
}
# handed_as_windows [<count>]: each of the programs the run started (six by default: the two
# testers, the two static shells and the stock shell twice) got Windows paths under the run's
# folder, a private USERPROFILE, SystemRoot, WINDIR and TEMP, and no variable of the caller's; no
# POSIX path.
handed_as_windows() {
  local each='USERPROFILE=C:\\temp\\run-e2e\.[^ ]*\\profile-[^ ]* .*'
  each+='TEMP=C:\\temp SystemRoot=C:\\Windows WINDIR=C:\\Windows LEAK=$'
  if [[ "$(grep -c "^\(unittest\.exe\|duckdb\.exe\) .*$each" "$handed")" != "${1:-6}" ]] ||
    grep -qF -- "$tmp" "$handed" || ! grep -q 'C:\\temp\\run-e2e\.' "$handed"; then
    echo "a POSIX path, or no private Windows one, was handed: $(cat "$handed")" >&2
    return 1
  fi
}
check "on Windows, every case passes with the set loaded under its generated names" \
  passes_on_windows
check "on Windows, the programs get Windows paths, a private profile and SystemRoot, WINDIR, TEMP" \
  handed_as_windows
check "on Windows, the cases leave nothing in the temporary folder" \
  test -z "$(ls -A "$drive/temp")"
check "on Windows, a missing cygpath fails the run before any case" \
  says "cygpath is required on Windows" env PATH="$win/uname-only:$PATH" bash "$script" "$win/build"
check "on Windows, a case host that is not loopback fails the run before any case" \
  before_any_case "every case must aim at a closed port on 127.0.0.1" \
  DUCKDB_REDSHIFT_TEST_HOST=10.0.0.1
check "on Windows, a case endpoint URL that is not loopback fails the run before any case" \
  before_any_case "every case must aim at a closed port on 127.0.0.1" \
  DUCKDB_REDSHIFT_TEST_ENDPOINT_URL=https://redshift.us-east-1.amazonaws.com:9
check "on Windows, other closed ports on 127.0.0.1 are taken" \
  passes_on_windows DUCKDB_REDSHIFT_TEST_ENDPOINT_URL=https://127.0.0.1:1
check "on Windows, an empty SystemRoot fails the run before any case" \
  before_any_case "SystemRoot or WINDIR is not set" SYSTEMROOT=
write_lock
check "on Windows, a lock that does not rename the set fails the run before any case" \
  before_any_case "does not rename each windows_amd64 set file"
write_lock "${renames[0]}" "${renames[2]}" "${renames[3]}"
check "on Windows, a lock that renames one of the set's two files fails the run before any case" \
  before_any_case "does not rename each windows_amd64 set file"
printf 'format 9\n' > "$lock"
check "on Windows, a lock that fails check-lock.sh fails the run before any case" \
  before_any_case "the build's lock fails check-lock.sh"
write_lock "${renames[@]}"
touch "$win/flags/folders-fail"
check "on Windows, folders it cannot read fail the run before any case" \
  before_any_case "could not read the temporary folder or the per-user cache"
rm "$win/flags/folders-fail"
mkdir -p "$drive/RUNNER~1" "$drive/runner-é" "$drive/$(printf 'r%.0s' {1..65})" \
  "$drive/$(printf 'r%.0s' {1..66})"
check "on Windows, a runner folder that does not exist fails the run before any case" \
  before_any_case "cannot make a folder under C:\\missing" REDSHIFT_E2E_RUNNER_DIR='C:\missing'
check "on Windows, a USERPROFILE with an 8.3 component fails before any case, as a runner path" \
  before_any_case "runner path: C:\\RUNNER~1\\" REDSHIFT_E2E_RUNNER_DIR='C:\RUNNER~1'
check "on Windows, a non-ASCII USERPROFILE fails before any case, as a runner path" \
  before_any_case "is not ASCII" REDSHIFT_E2E_RUNNER_DIR='C:\runner-é'
# The longest handed path is the loadable copy's, 52 characters below the base.
check "on Windows, a path over the length limit fails before any case, as a runner path" \
  before_any_case "is longer than 120 characters" \
  REDSHIFT_E2E_RUNNER_DIR="C:\\$(printf 'r%.0s' {1..66})"
check "on Windows, paths of the length limit are taken" \
  passes_on_windows REDSHIFT_E2E_RUNNER_DIR="C:\\$(printf 'r%.0s' {1..65})"
check "on Windows, a tester that writes no set fails no-network" \
  flagged no-set "no-network: the driver set was not written"
check "on Windows, a set variable that does not stop the write fails refused-variable" \
  flagged refused-writes "refused-variable: the driver directory was written to"
check "on Windows, a shell that does not exit as after a failed statement fails its case" \
  flagged clean-exit "static-shell-default-base: the shell's run failed"
check "on Windows, a connect that fails before the driver fails its case" \
  flagged refused "static-shell-default-base: the driver did not load and fail the connect"
check "on Windows, a set file loaded from elsewhere fails" \
  flagged loaded-elsewhere "static-shell-default-base: abcdefghijk23456.dll is loaded 0 times"
check "on Windows, a set file loaded under its original name fails" \
  flagged original "static-shell-default-base: c:\\temp\\"
check "on Windows, a case without its trust file fails" \
  flagged no-trust "static-shell-default-base: no trust\\<SHA-256>.pem"
check "on Windows, a trust file not named by its SHA-256 fails" \
  flagged wrong-trust "static-shell-default-base: no trust\\<SHA-256>.pem"
check "on Windows, a case that writes into the per-user cache's trust folder fails" \
  flagged cache "static-shell-default-base: the run wrote into the per-user cache's trust folder"
cache="$drive/cache/profile-static-shell-default-base/duckdb-redshift/trust"
check "on Windows, what the case wrote into its profile's per-user cache is removed" test ! -e "$cache"
mkdir -p "$cache"; : > "$cache/earlier.pem"; touch "$win/flags/cache"
check "on Windows, a shell run that fails after writing into the per-user cache fails its case" \
  flagged clean-exit "static-shell-default-base: the shell's run failed"
rm "$win/flags/cache"
kept_earlier() { [[ -e "$cache/earlier.pem" && ! -e "$cache/new.pem" ]]; }
check "on Windows, that failed run removes only what it wrote, keeping an earlier file" kept_earlier
rm -rf "${drive:?}/cache/profile-static-shell-default-base"
# A name holding a newline lists as two lines; the bare second one must not be removed from the
# folder the run started in.
mkdir -p "$tmp/cwd/kept"
newline_kept() {
  (cd "$tmp/cwd" && flagged cache-newline "the run wrote into the per-user cache's trust folder") &&
    [[ -d "$tmp/cwd/kept" ]]
}
check "on Windows, a cache name holding a newline removes nothing outside the trust folder" newline_kept
rm -rf "${drive:?}/cache/profile-static-shell-default-base"
check "on Windows, a case whose per-user cache cannot be read fails before its shell runs" \
  flagged case-folders-fail "static-shell-default-base: windows_modules.py could not read the case's"
check "on Windows, a planted file in the module list fails planted-dlls" \
  flagged planted "planted-dlls: the shell loaded the planted"
check "on Windows, a planted copy loaded under another path and name fails planted-dlls" \
  flagged planted-copy "planted-dlls: the shell loaded a planted copy as"
check "on Windows, a set file loaded twice fails" \
  flagged doubled "static-shell-default-base: abcdefghijk23456.dll is loaded 2 times"
check \
  "on Windows, with the DbgHelp.dll check on, a case without System32's DbgHelp.dll fails" \
  flagged no-dbghelp "static-shell-default-base: DbgHelp.dll is loaded 0 times, not once"
check "on Windows, an INSTALL that puts two copies in the extension directory fails" \
  flagged twice-install "loadable-extension: INSTALL did not put one copy"
check "on Windows, an empty WINDIR fails the run before any case" \
  before_any_case "SystemRoot or WINDIR is not set" WINDIR=
check "on Windows, one DbgHelp.dll from outside System32 fails" \
  flagged dbghelp-elsewhere "static-shell-default-base: c:\\temp\\dbghelp.dll is loaded, not System32's DbgHelp.dll"
header="$win/build/extension/redshift/redshift_driver/driver_info.h"
mv "$header" "$header.kept"
check "on Windows, a build with no driver_info.h fails the run before any case" \
  before_any_case "the build has no $header"
# Windows' Python writes the lock rows and the header with CRLF line ends, which every read drops.
printf '#define REDSHIFT_DRIVER_SYSTEM_DBGHELP 1\r\n' > "$header"
touch "$win/flags/crlf"
check "on Windows, CRLF lock rows and header pass" passes_on_windows
check "on Windows, a CRLF header still turns the DbgHelp.dll check on" \
  flagged no-dbghelp "static-shell-default-base: DbgHelp.dll is loaded 0 times, not once"
rm "$win/flags/crlf"
# A build whose loader does not check DbgHelp.dll records 0, and then no count of it is checked.
printf '#define REDSHIFT_DRIVER_SYSTEM_DBGHELP 0\n' > "$header"
touch "$win/flags/no-dbghelp"
check "on Windows, a header that turns the DbgHelp.dll check off lets a run that loads none pass" \
  passes_on_windows
rm "$win/flags/no-dbghelp"; rm -rf -- "${win:?}/stock/.redshift-driver" "${win:?}/stock/trust"
mv "$header.kept" "$header"
printf '%s\n' "format 2" "version 2.3.0.0" "status development" \
  "linux_amd64  driver  librsodbc64.so  $h1  $rpm  $h2  $so" \
  "*  notice  LICENSE  $h3  $notices/LICENSE" "*  notice  NOTICE  $h3  $notices/NOTICE" \
  "*  notice  THIRD_PARTY_LICENSES  $h3  $notices/THIRD_PARTY_LICENSES" > "$lock"
check "on Windows, a lock with no windows_amd64 row fails the run before any case" \
  before_any_case "does not rename each windows_amd64 set file"
write_lock "${renames[@]}"
no_stock() {
  local out
  out="$(env PATH="$win/bin:$PATH" SYSTEMROOT='C:\Windows' WINDIR='C:\Windows' \
    bash "$script" "$win/build" 2>&1)" ||
    { echo "it failed: $out" >&2; return 1; }
  grep -qF "run-e2e: loadable-extension # SKIP no stock DuckDB shell given" <<<"$out"
}
check "on Windows, without a stock shell the loadable-extension case is a SKIP and the run passes" \
  no_stock
check "on Windows, an INSTALL that puts no copy in the extension directory fails" \
  flagged no-install "loadable-extension: INSTALL did not put one copy"
check "on Windows, a set written beside the copy INSTALL read fails" \
  flagged beside-copy "loadable-extension: the set was written beside the copy INSTALL read"
check "on Windows, a set written beside the test host and not the installed file fails" \
  flagged beside-host "run-e2e: loadable-extension: the set was not written to"

# The second process (loadable-extension-again) must find the first run's trust file and write no
# other; the stand-in shell writes a second one under the flag.
check "on Windows, a second process that writes another trust file fails" \
  flagged other-trust "loadable-extension-again: the second process did not reuse the trust file"
# Only the second process misbehaves under these flags, so each check below is that case's own.
check "on Windows, a second process that loads the set from elsewhere fails" \
  flagged again-elsewhere "loadable-extension-again: abcdefghijk23456.dll is loaded 0 times"
check "on Windows, a second process that sets the set aside and writes it again fails" \
  flagged again-set-aside "loadable-extension-again: the second process did not reuse the set"
check "on Windows, a second process that rewrites a file of the set in place fails" \
  flagged again-rewrite "changed since the first case: $drive/"
again_same_profile() {
  # Both loadable-extension runs must get the first case's profile, or the second process proves
  # nothing about reuse. A fresh run first, so $handed holds this run's records alone.
  local two
  passes_on_windows || return 1
  two="$(grep -c 'USERPROFILE=C:\\temp\\run-e2e\.[^ ]*\\profile-loadable-extension ' "$handed")"
  [[ "$two" == 2 ]] || { echo "the two runs' profiles: $two of 2 ($(cat "$handed"))" >&2; return 1; }
}
check "on Windows, the second process gets the first case's profile, not one of its own" \
  again_same_profile
# The read-only tree an administrator provisions (REDSHIFT_E2E_ADMIN_DIR): the set is there, this
# account cannot write it, and the trust file goes to the per-user cache.
admin="$drive/admin-tree"
mkdir -p "$admin/.redshift-driver/set"
: > "$admin/.redshift-driver/set/abcdefghijk23456.dll"; : > "$admin/.redshift-driver/set/bcdefghijk234567.dll"
chmod 555 "$admin"
# flagged_tree <flag> <message>: flagged_admin, then the tree's set put back as it was.
flagged_tree() {
  local status=0
  flagged_admin "$@" || status=1
  rm -f -- "$admin/.redshift-driver/set/added.dll"
  : > "$admin/.redshift-driver/set/abcdefghijk23456.dll"; : > "$admin/.redshift-driver/set/bcdefghijk234567.dll"
  return "$status"
}
admin_runs() {
  local status=0
  want_skip=""
  passes_on_windows REDSHIFT_E2E_ADMIN_DIR='C:\admin-tree' "$@" || status=1
  want_skip=read-only-admin-tree
  return "$status"
}
admin_no_stock() {
  local out
  out="$(env PATH="$win/bin:$PATH" SYSTEMROOT='C:\Windows' WINDIR='C:\Windows' \
    REDSHIFT_E2E_ADMIN_DIR='C:\admin-tree' bash "$script" "$win/build" 2>&1)" ||
    { echo "it failed: $out" >&2; return 1; }
  [[ "$out" == *"run-e2e: read-only-admin-tree"* && "$out" != *"read-only-admin-tree # SKIP"* ]] ||
    { echo "the case did not run without a stock shell: $out" >&2; return 1; }
}
cache_trust="$drive/cache/profile-admin-tree/duckdb-redshift/trust"
if [[ -w "$admin" ]]; then
  # As root (or under unshare -r) a mode-555 folder does not stop the write probe, so the cases
  # that need a tree this account cannot write cannot run here; the refusals below still do.
  for name in "the read-only administrator tree's case runs when the runner names one" \
    "that case hands the tree and its own profile to the shell as Windows paths" \
    "the case's trust file in the per-user cache is removed after it" \
    "REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1 with the tree named runs the case and passes" \
    "a case that writes no trust file into the per-user cache fails" \
    "a case that writes two files into the per-user cache fails" \
    "the read-only tree's case runs without a stock shell, which it does not use" \
    "a cache trust file not named by its SHA-256 fails the read-only case" \
    "a set loaded from outside the administrator tree fails its case" \
    "a case that adds a file to the administrator tree fails" \
    "a case that rewrites a file of the administrator tree in place fails"; do
    skip "on Windows, $name: this account writes a mode-555 folder (root)"
  done
else
  check "on Windows, the read-only administrator tree's case runs when the runner names one" \
    admin_runs
  check "on Windows, that case hands the tree and its own profile to the shell as Windows paths" \
    handed_as_windows 7
  check "on Windows, the case's trust file in the per-user cache is removed after it" \
    test ! -e "$cache_trust"
  check "on Windows, REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1 with the tree named runs the case and passes" \
    admin_runs REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1
  check "on Windows, a case that writes no trust file into the per-user cache fails" \
    flagged_admin no-cache-trust "read-only-admin-tree: the case left nothing in the per-user cache"
  check "on Windows, a case that writes two files into the per-user cache fails" \
    flagged_admin two-cache-trust "read-only-admin-tree: the case left"
  check "on Windows, the read-only tree's case runs without a stock shell, which it does not use" \
    admin_no_stock
  check "on Windows, a cache trust file not named by its SHA-256 fails the read-only case" \
    flagged_admin admin-wrong-trust "read-only-admin-tree: the case left"
  check "on Windows, a set loaded from outside the administrator tree fails its case" \
    flagged_admin admin-elsewhere "read-only-admin-tree: abcdefghijk23456.dll is loaded 0 times"
  check "on Windows, a case that adds a file to the administrator tree fails" \
    flagged_tree admin-add "read-only-admin-tree: the case changed the provisioned tree"
  check "on Windows, a case that rewrites a file of the administrator tree in place fails" \
    flagged_tree admin-rewrite "read-only-admin-tree: the case changed the provisioned tree"
fi
chmod 755 "$admin"
check "on Windows, a writable administrator tree fails its case" \
  says "read-only-admin-tree: this account can write in C:\\admin-tree" \
  windows_run REDSHIFT_E2E_ADMIN_DIR='C:\admin-tree'
# A probe file an earlier run left behind does not make a writable tree look read-only.
: > "$admin/.redshift-e2e-write-probe"
check "on Windows, a writable administrator tree with an earlier probe's file in it fails its case" \
  says "read-only-admin-tree: this account can write in C:\\admin-tree" \
  windows_run REDSHIFT_E2E_ADMIN_DIR='C:\admin-tree'
rm -f -- "$admin/.redshift-e2e-write-probe"
[[ -z "$(find "$admin" -maxdepth 1 -name '.redshift-e2e-write-probe.*')" ]] ||
  fail "the write probe left a file in the administrator tree"
chmod 555 "$admin"
rm -rf -- "${drive:?}/no-set-tree"; mkdir -p "$drive/no-set-tree"; chmod 555 "$drive/no-set-tree"
check "on Windows, an administrator tree without the set fails its case" \
  says "read-only-admin-tree: C:\\no-set-tree holds no .redshift-driver" \
  windows_run REDSHIFT_E2E_ADMIN_DIR='C:\no-set-tree'
chmod 755 "$drive/no-set-tree"
check "on Windows, an administrator tree that is not a folder fails before any case" \
  before_any_case "REDSHIFT_E2E_ADMIN_DIR names C:\\missing-tree, which is not a folder" \
  REDSHIFT_E2E_ADMIN_DIR='C:\missing-tree'
# A host that provisioned the tree requires its case: with none named the run fails, not skips.
check "on Windows, REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1 with no tree named fails before any case" \
  before_any_case "REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1, but REDSHIFT_E2E_ADMIN_DIR names no" \
  REDSHIFT_E2E_REQUIRE_ADMIN_DIR=1
for value in yes true 2; do
  check "on Windows, REDSHIFT_E2E_REQUIRE_ADMIN_DIR=$value fails before any case" \
    before_any_case "REDSHIFT_E2E_REQUIRE_ADMIN_DIR is '$value'; it is 1 to require" \
    REDSHIFT_E2E_REQUIRE_ADMIN_DIR="$value" REDSHIFT_E2E_ADMIN_DIR='C:\admin-tree'
done
check "on Windows, REDSHIFT_E2E_REQUIRE_ADMIN_DIR=0 lets the read-only case skip" \
  passes_on_windows REDSHIFT_E2E_REQUIRE_ADMIN_DIR=0
chmod 755 "$admin"

((failed == 0)) || { echo "$failed of $n failed" >&2; exit 1; }
echo "all $n passed"
