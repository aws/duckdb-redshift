#!/usr/bin/env bash
# Tool tests for the Windows set's rename: tools/lib/rename_pin.py, rename_apply.py, rename_check.py
# and rename-tools.sh. Every image is a synthetic PE32+ that make_pe.py writes at
# test time. The pin's own steps run in a copy of the tools whose requirements pin two wheels built
# here, one holding fake_dll_utils.py in delvewheel's place, which a stand-in curl serves at their
# pinned URLs, so nothing is downloaded; with RS_RENAME_TOOLS holding the real pinned wheels, the
# last case runs the checked-in tools on the same set, and with RS_REQUIRE_RENAME_TOOLS=1 a missing
# wheel fails it instead of skipping. Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
odbc="$here/../../../third_party/redshift_odbc"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

# rename_pin.py pin reads the wheels with importlib.metadata, so an older python3 skips the suite.
if ! python3 -I -c 'import sys; sys.exit(sys.version_info < (3, 8))'; then
  if [[ "${RS_REQUIRE_RENAME_TOOLS:-}" == 1 ]]; then
    echo "not ok 1 - rename_pin.py pin needs Python 3.8 or later; RS_REQUIRE_RENAME_TOOLS=1 is set"
    exit 1
  fi
  echo "ok 1 - the rename's tools # SKIP rename_pin.py pin needs Python 3.8 or later"; exit 0
fi

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# shellcheck source=bounded.sh
source "$here/bounded.sh"
sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
pe() { python3 -I "$here/make_pe.py" "$@"; }
# same <a> <b>: two files, or two directories' files, hold the same bytes (cmp or diff may be missing).
same() {
  python3 -I -c 'import os, sys
def read(p):
    return {f: open(os.path.join(p, f), "rb").read() for f in os.listdir(p)} if os.path.isdir(p) else open(p, "rb").read()
sys.exit(read(sys.argv[1]) != read(sys.argv[2]))' "$1" "$2"
}
# expect <name> <exit> <output-substring> <command>...: the command's exit status, and its output
# (stdout and stderr) holding the substring.
expect() {
  local name="$1" want="$2" text="$3" out status
  shift 3
  out="$(bounded "$@" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$text"* ]]; then pass "$name"
  else fail "$name (exit $status, wanted $want; output: ${out:0:400})"; fi
}

# The copy of the tools, its requirements pinning the two test wheels at stand-in URLs, and the
# directory the stand-in curl serves them from.
mkdir -p "$tmp/tree/third_party/redshift_odbc" "$tmp/served" "$tmp/wheels" "$tmp/empty" "$tmp/bin"
cp -R "$odbc/tools" "$odbc/policy" "$tmp/tree/third_party/redshift_odbc/"
lib="$tmp/tree/third_party/redshift_odbc/tools/lib"
python3 -I - "$tmp/wheels" "$here/fake_dll_utils.py" <<'PY'
import sys, zipfile
out, fake = sys.argv[1], open(sys.argv[2]).read()
for name, version, files in (("delvewheel", "1.10.0", {"delvewheel/__init__.py": "", "delvewheel/_dll_utils.py": fake}),
                             ("pefile", "2024.8.26", {"pefile.py": ""})):
    with zipfile.ZipFile("%s/%s.whl" % (out, name), "w") as z:
        for path, text in files.items():
            z.writestr(path, text)
        z.writestr("%s-%s.dist-info/METADATA" % (name, version), "Metadata-Version: 2.1\nName: %s\nVersion: %s\n" % (name, version))
        z.writestr("%s-%s.dist-info/WHEEL" % (name, version), "Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n")
PY
dw="$(sha "$tmp/wheels/delvewheel.whl")"; pf="$(sha "$tmp/wheels/pefile.whl")"
cp "$tmp/wheels/delvewheel.whl" "$tmp/wheels/pefile.whl" "$tmp/served/"
dw_url=https://files.example/delvewheel.whl; pf_url=https://files.example/pefile.whl
# requirements <delvewheel version> [<pin options>]: the copy's requirements file.
requirements() {
  printf 'delvewheel==%s %s\npefile==2024.8.26 --hash=sha256:%s --url=%s\n' "$1" \
    "${2---hash=sha256:$dw --url=$dw_url}" "$pf" "$pf_url" > "$lib/rename-requirements.txt"
}
requirements 1.10.0
# The caller's cache of the real pinned wheels, for the last case; rename_pin.py's default if unset.
real_tools="${RS_RENAME_TOOLS:-$here/../../../build/rename-tools}"
export RS_RENAME_TOOLS="$tmp/tools"
unset RS_DRIVER_MIRROR RS_FAKE_DELVEWHEEL_GROW RS_FAKE_DELVEWHEEL_CLEAR_FLAGS STAND_IN_SERVES
# Nothing here downloads: download.sh runs this curl, which records each URL in $STAND_IN_LOG and
# its options in $STAND_IN_ARGS, and serves the file named as the URL's last part from
# $STAND_IN_SERVES, or answers 404.
cat > "$tmp/bin/curl" <<'SH'
#!/usr/bin/env bash
echo "$*" >> "$STAND_IN_ARGS"
out="" url=""
while (($#)); do
  case "$1" in
  --output) out="$2"; shift 2 ;;
  --) url="$2"; break ;;
  *) shift ;;
  esac
done
echo "$url" >> "$STAND_IN_LOG"
file="${STAND_IN_SERVES:-/nonexistent}/${url##*/}"
if [[ ! -f "$file" ]]; then
  printf 404; echo "curl: (22) The requested URL returned error: 404" >&2; exit 22
fi
cat "$file" > "$out" && printf 200
SH
chmod +x "$tmp/bin/curl"
export PATH="$tmp/bin:$PATH" STAND_IN_LOG="$tmp/curl.log" STAND_IN_ARGS="$tmp/curl-args.log"
# asked: the URLs the stand-in curl was asked for since the log was last emptied, on one line.
asked() { tr '\n' ' ' < "$STAND_IN_LOG"; }
# verdict <name> <command>...: passes when the command succeeds, else fails naming what was asked.
verdict() {
  local name="$1"; shift
  if "$@"; then pass "$name"; else fail "$name (asked: $(asked))"; fi
}
: > "$STAND_IN_LOG"
pe "$tmp/set-seed.dll" '{"rdata": {"imports": ["KERNEL32.dll", "b.dll"]}}'
policy="$lib/../../policy/windows/unshipped-names.txt"
pin=(python3 -I "$lib/rename_pin.py"); apply=(python3 -I "$lib/rename_apply.py"); check=(python3 -I "$lib/rename_check.py")

expect "fetch-tools downloads each wheel from its pinned URL, whatever RS_DRIVER_MIRROR says" 0 "" \
  env RS_DRIVER_MIRROR="$tmp/empty" STAND_IN_SERVES="$tmp/served" "${pin[@]}" fetch-tools
both_cached() {
  [[ "$(asked)" == "$dw_url $pf_url " && "$(sha "$tmp/tools/$dw.whl")" == "$dw" &&
    "$(sha "$tmp/tools/$pf.whl")" == "$pf" ]]
}
verdict "the cache holds both wheels, from the two pinned URLs alone" both_cached
# Each fetch went through download.sh's bounds.
bounded_fetches() {
  awk 'index($0, "--proto =https") && index($0, "--max-redirs 0") &&
    index($0, "--max-filesize 1048576") { ok++ } END { exit !(ok == 2 && NR == 2) }' "$STAND_IN_ARGS"
}
verdict "each wheel is fetched over HTTPS only, with no redirect, and at most 1 MiB" bounded_fetches
: > "$STAND_IN_LOG"
expect "fetch-tools downloads nothing once the cache holds the pinned wheels" 0 "" \
  "${pin[@]}" fetch-tools
verdict "a warm cache asks for no URL" test -z "$(asked)"
mkdir "$tmp/bad"
cp "$tmp/wheels/pefile.whl" "$tmp/bad/delvewheel.whl"; cp "$tmp/wheels/pefile.whl" "$tmp/bad/"
expect "fetch-tools refuses a downloaded wheel whose SHA-256 is not the pinned one" 1 \
  "tools: $dw_url is not the pinned delvewheel==1.10.0: its SHA-256 is $pf" \
  env STAND_IN_SERVES="$tmp/bad" RS_RENAME_TOOLS="$tmp/bad-cache" "${pin[@]}" fetch-tools
verdict "a refused download leaves nothing in the cache" test -z "$(ls -A "$tmp/bad-cache")"
expect "fetch-tools fails when a pinned URL cannot be downloaded, naming it" 1 \
  "tools: downloading delvewheel==1.10.0 from $dw_url failed" \
  env STAND_IN_SERVES="$tmp/empty" RS_RENAME_TOOLS="$tmp/cache-404" "${pin[@]}" fetch-tools
mkdir "$tmp/no-bash"
expect "fetch-tools without bash on PATH fails in one line, not a traceback" 1 \
  "tools: cannot run $lib/download.sh: [Errno 2]" \
  env PATH="$tmp/no-bash" RS_RENAME_TOOLS="$tmp/cache-no-bash" "$(command -v python3)" -I \
  "$lib/rename_pin.py" fetch-tools
: > "$tmp/cache-file"
expect "fetch-tools fails in one line when its cache cannot be made" 1 \
  "tools: cannot create $tmp/cache-file/x:" \
  env RS_RENAME_TOOLS="$tmp/cache-file/x" "${pin[@]}" fetch-tools
# A cache folder the user cannot write; root writes it anyway, so then the case skips.
mkdir "$tmp/read-only-cache"; chmod 555 "$tmp/read-only-cache"
if [[ -w "$tmp/read-only-cache" ]]; then
  n=$((n+1)); echo "ok $n - fetch-tools fails in one line when it cannot write into its cache # SKIP this user writes a mode-555 folder (root)"
else
  expect "fetch-tools fails in one line when it cannot write into its cache" 1 \
    "tools: writing $tmp/read-only-cache/$dw.whl failed: [Errno 13]" \
    env STAND_IN_SERVES="$tmp/served" RS_RENAME_TOOLS="$tmp/read-only-cache" "${pin[@]}" fetch-tools
fi
chmod 755 "$tmp/read-only-cache"
mkdir "$tmp/stale"; cp "$tmp/wheels/pefile.whl" "$tmp/stale/$dw.whl"; : > "$STAND_IN_LOG"
expect "fetch-tools refuses a cached wheel whose bytes are not the pinned ones" 1 \
  "tools: $tmp/stale/$dw.whl is not the pinned delvewheel==1.10.0; remove it to fetch it again" \
  env STAND_IN_SERVES="$tmp/served" RS_RENAME_TOOLS="$tmp/stale" "${pin[@]}" fetch-tools
verdict "a refused cached wheel is kept, and nothing is fetched" \
  test -z "$(asked)" -a "$(sha "$tmp/stale/$dw.whl")" == "$pf"
requirements 1.10.0 "--hash=sha256:$dw"
expect "fetch-tools refuses a requirement without a pinned URL" 1 \
  "delvewheel==1.10.0 has no --url pin" env RS_RENAME_TOOLS="$tmp/unused" "${pin[@]}" fetch-tools
requirements 1.10.0 "--hash=sha256:$dw --url=http://files.example/delvewheel.whl"
expect "the tools refuse a pinned URL that is not https://" 1 "is not <name>==<version>" \
  env RS_RENAME_TOOLS="$tmp/unused" "${pin[@]}" fetch-tools
requirements 1.10.0 ""
expect "fetch-tools refuses a requirement without a pinned SHA-256" 1 \
  "delvewheel==1.10.0 has no --hash=sha256 pin" \
  env RS_RENAME_TOOLS="$tmp/unused" "${pin[@]}" fetch-tools
mkdir "$tmp/one"; cp "$tmp/set-seed.dll" "$tmp/one/a.dll"; cp "$tmp/set-seed.dll" "$tmp/one/b.dll"
expect "pin exits 3 for a requirement without a pinned SHA-256" 3 "delvewheel==1.10.0 has no --hash=sha256 pin" \
  "${pin[@]}" pin "$tmp/one" a.dll
printf 'delvewheel>=1.10.0\n' > "$lib/rename-requirements.txt"
expect "the tools refuse a requirement that is not name==version" 1 \
  "'delvewheel>=1.10.0' is not <name>==<version>" "${pin[@]}" fetch-tools
printf 'pefile==2024.8.26 --hash=sha256:%s --url=%s\n' "$pf" "$pf_url" \
  > "$lib/rename-requirements.txt"
expect "the tools refuse requirements that pin no delvewheel" 1 "pins no delvewheel" "${pin[@]}" fetch-tools
requirements 1.10.0
expect "fetch-tools fills build/rename-tools by default" 0 "2" \
  env -u RS_RENAME_TOOLS STAND_IN_SERVES="$tmp/served" \
  bash -c 'python3 -I "$1" fetch-tools && ls "$2" | grep -c "\.whl$"' \
  _ "$lib/rename_pin.py" "$tmp/tree/build/rename-tools"
# A wheel over rename_pin.py's 1 MiB bound, pinned by its own SHA-256, so only the bound refuses it.
mkdir "$tmp/big"; cp "$tmp/wheels/pefile.whl" "$tmp/big/"
python3 -I -c 'import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("pad", bytes((1 << 20) + 4096))' "$tmp/big/delvewheel.whl"
requirements 1.10.0 "--hash=sha256:$(sha "$tmp/big/delvewheel.whl") --url=$dw_url"
expect "fetch-tools refuses a pinned wheel larger than 1 MiB" 1 "the 1048576-byte size bound" \
  env STAND_IN_SERVES="$tmp/big" RS_RENAME_TOOLS="$tmp/big-cache" "${pin[@]}" fetch-tools
requirements 1.10.0
# A directory at the wheel's name makes the replace fail: one line, and no .part left behind.
mkdir -p "$tmp/blocked/$dw.whl"
expect "fetch-tools fails in one line when the wheel cannot be written" 1 \
  "tools: writing $tmp/blocked/$dw.whl failed:" \
  env STAND_IN_SERVES="$tmp/served" RS_RENAME_TOOLS="$tmp/blocked" "${pin[@]}" fetch-tools
verdict "a failed write leaves no .part in the cache" \
  bash -c '! compgen -G "$1/*.part" > /dev/null' _ "$tmp/blocked"
for tool in rename_pin rename_apply rename_check; do
  expect "$tool.py prints its usage without its arguments" 2 "usage: $tool.py" python3 -I "$lib/$tool.py"
done
# The cache may be another process's to write: the fetch writes no file or link already there, and
# the pin imports a private copy of the bytes it checked.
mkdir "$tmp/linked"; ln -s "$tmp/victim" "$tmp/linked/$dw.whl"
expect "fetch-tools replaces a dangling link at the wheel's name, writing through no link" 0 "absent" \
  env STAND_IN_SERVES="$tmp/served" RS_RENAME_TOOLS="$tmp/linked" bash -c \
  'python3 -I "$1" fetch-tools && [[ -f "$2" && ! -L "$2" ]] && { [[ -e "$3" ]] && echo present || echo absent; }' \
  _ "$lib/rename_pin.py" "$tmp/linked/$dw.whl" "$tmp/victim"
expect "pin imports delvewheel from a private copy of the checked wheel, not from the cache" 0 \
  "private" env RS_RENAME_TOOLS="$tmp/linked" python3 -I -c 'import sys
sys.path.insert(0, sys.argv[1]); import rename_pin; rename_pin.load_tools()
from delvewheel import _dll_utils
print("cache" if _dll_utils.__file__.startswith(sys.argv[2]) else "private")' "$lib" "$tmp/linked"

# rename-tools.sh, as the Linux make test_driver recipe runs it, in the copy of the tree: only a
# lock with a rename row fetches the tools, and it prints the switch that makes the pinned-tool
# cases required.
mkdir -p "$tmp/tree/test/client/driver"
cp "$here/rename-tools.sh" "$tmp/tree/test/client/driver/"
rename_tools="$tmp/tree/test/client/driver/rename-tools.sh"
printf 'format 2\nwindows_amd64  rename  krb5_64.dll  abcdefg.dll\n' > "$tmp/renaming.lock"
printf 'format 1\n# rename rows come with the Windows set\n' > "$tmp/plain.lock"
expect "rename-tools.sh fetches the tools for a lock with a rename row and prints 1" 0 \
  "switch=1 wheels=2" env STAND_IN_SERVES="$tmp/served" RS_RENAME_TOOLS="$tmp/fetched" bash -c \
  'echo "switch=$(bash "$1" "$2") wheels=$(ls "$RS_RENAME_TOOLS" | wc -l | tr -d " ")"' \
  _ "$rename_tools" "$tmp/renaming.lock"
expect "rename-tools.sh fails for a lock with a rename row when the tools cannot be fetched" 1 \
  "rename-tools: cannot fetch the pinned rename tools for $tmp/renaming.lock; offline, put delvewheel's and pefile's wheels in $tmp/fetched2/ as <sha256>.whl" \
  env STAND_IN_SERVES="$tmp/empty" RS_RENAME_TOOLS="$tmp/fetched2" \
  bash "$rename_tools" "$tmp/renaming.lock"
: > "$STAND_IN_LOG"
expect "rename-tools.sh fetches and prints nothing for a lock without a rename row" 0 \
  "switch= asked=" env RS_RENAME_TOOLS="$tmp/never" bash -c \
  'echo "switch=$(bash "$1" "$2") asked=$(cat "$STAND_IN_LOG")"' _ "$rename_tools" "$tmp/plain.lock"
verdict "rename-tools.sh makes no cache for a lock without a rename row" test ! -e "$tmp/never"
expect "rename-tools.sh without its one argument is a usage error" 2 \
  "usage: rename-tools.sh <lock>" bash "$rename_tools"
expect "rename-tools.sh fails on a lock it cannot read" 1 \
  "rename-tools: cannot read the lock $tmp/no.lock" bash "$rename_tools" "$tmp/no.lock"

# run-tool-tests.sh in a copy whose suites are empty stand-ins, and one that prints the switch, so
# only its own steps run: it fetches nothing, and the switch is its caller's.
rtt="$tmp/rtt/test/client/driver"
mkdir -p "$rtt"
cp "$here/run-tool-tests.sh" "$rtt/"
for suite in $(sed -n '/^for required in/,/; do$/p' "$here/run-tool-tests.sh" | grep -o '[a-z0-9-]*_test\.sh'); do
  : > "$rtt/$suite"
done
echo 'echo "require=${RS_REQUIRE_RENAME_TOOLS:-}"' > "$rtt/zz-switch_test.sh"
: > "$STAND_IN_LOG"
expect "run-tool-tests.sh fetches nothing for a renaming lock and sets no switch" 0 \
  "require= asked=" env -u RS_REQUIRE_RENAME_TOOLS RS_DRIVER_LOCK="$tmp/renaming.lock" \
  RS_RENAME_TOOLS="$tmp/rtt-cache" bash -c \
  'echo "$(bash "$1" | grep "^require=") asked=$(cat "$STAND_IN_LOG")"' _ "$rtt/run-tool-tests.sh"
verdict "run-tool-tests.sh makes no rename-tools cache" test ! -e "$tmp/rtt-cache"
expect "run-tool-tests.sh passes its caller's RS_REQUIRE_RENAME_TOOLS=1 to the suites" 0 \
  "require=1" env RS_REQUIRE_RENAME_TOOLS=1 RS_DRIVER_LOCK="$tmp/renaming.lock" \
  bash "$rtt/run-tool-tests.sh"

# notices <dir>: the lock's three notices, which an original folder holds beside the DLLs.
notices() {
  local notice
  for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do echo "$notice" > "$1/$notice"; done
}
# A set as the 2.3.0.0 MSI's is described: the driver imports the libraries, krb5_64 holds its own
# name for a self-lookup and the unshipped DLLs' names and imports one (an import name stays as
# it is), msvcr100 is signed and changes no byte; the notices are beside them.
set_files() { # set_files <dir> <crypto's string>
  mkdir -p "$1"
  pe "$1/rsodbc.dll" '{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll", "KRB5_64.dll"], "export_name": "rsodbc.dll", "load_flags": 2304}, "checksum": "valid"}'
  pe "$1/crypto.dll" "{\"rdata\": {\"imports\": [\"KERNEL32.dll\"], \"strings\": [[\"a\", \"$2\"]]}}"
  pe "$1/krb5_64.dll" '{"rdata": {"imports": ["KERNEL32.dll", "msvcr100.dll", "krbcc64.dll"], "export_name": "krb5_64.dll", "strings": [["a", "krb5_64"], ["a", "KRBCC64"], ["u", "leashw64.dll"]]}, "resources": [[16, 1, 1033, "k\u0000r\u0000b\u00005\u0000_\u00006\u00004\u0000.\u0000d\u0000l\u0000l\u0000\u0000\u0000"]]}'
  pe "$1/msvcr100.dll" '{"rdata": {"imports": ["KERNEL32.dll"], "export_name": "MSVCR100_CLR0400.dll"}, "checksum": "valid", "certificate": "signed attributes name msvcr100.dll\u0000"}'
  notices "$1"
}
set_files "$tmp/set" "OpenSSL 3"
expect "pin writes the set's rows with the fake routine" 0 "" bash -c 'python3 -I "$1" pin "$2" rsodbc.dll > "$3"' \
  _ "$lib/rename_pin.py" "$tmp/set" "$tmp/rows"
if [[ "$(awk '$2 == "driver" || $2 == "library" { print $2, $3 }' "$tmp/rows" | tr '\n' ' ')" == \
  "library crypto.dll library krb5_64.dll library msvcr100.dll driver rsodbc.dll " ]]; then
  pass "pin writes a driver row for the named driver and a library row for each other DLL"
else fail "pin writes a driver row for the named driver and a library row for each other DLL"; fi
expect "the applier reproduces the set from the rows" 0 "" "${apply[@]}" "$tmp/rows" "$tmp/set" "$tmp/out"
expect "the checker passes the applier's output" 0 "" "${check[@]}" "$tmp/rows" "$tmp/set" "$tmp/out"
# The checker recomputes the name seed from the original folder, which must hold the three notices,
# and the transform's identity in its own tree: its tools' lines and the pin's and applier's
# versions, so a seed that any other file, tool line or version gives fails, though every name
# derives from it. The pass above shows its seed equal to the pin's for a set with rewritten strings.
seed_case() { # seed_case <name> <exit> <text> <original folder>
  expect "$1" "$2" "$3" "${check[@]}" "$tmp/rows" "$4" "$tmp/out"
}
seed_differs="seed: the name-seed row $(awk '$2 == "name-seed" { print $3 }' "$tmp/rows") is not"
cp -R "$tmp/set" "$tmp/seed-notice"; echo "another notice" > "$tmp/seed-notice/LICENSE"
seed_case "the checker refuses a notice other than the one the seed covers" 1 "$seed_differs" "$tmp/seed-notice"
for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
  cp -R "$tmp/set" "$tmp/seed-no-$notice"; rm "$tmp/seed-no-$notice/$notice"
  seed_case "the checker refuses an original folder without $notice, naming it" 1 \
    "seed: $tmp/seed-no-$notice holds no $notice, one of the notices the seed covers" "$tmp/seed-no-$notice"
done
cp -R "$tmp/set" "$tmp/seed-extra"; cp "$tmp/set/crypto.dll" "$tmp/seed-extra/extra.dll"
seed_case "the checker refuses an original folder holding a DLL no row names" 1 \
  "rename: $tmp/seed-extra holds extra.dll, which no driver or library row names" "$tmp/seed-extra"
mkdir "$tmp/seed-folder"; cp -R "$tmp/set/." "$tmp/seed-folder/"; mkdir "$tmp/seed-folder/sub"
seed_case "the checker refuses an original folder holding a folder" 1 "seed: $tmp/seed-folder/sub is not a regular file" \
  "$tmp/seed-folder"
# A name the lock refuses, which could add a line of its own to the manifest the seed covers.
cp -R "$tmp/set" "$tmp/seed-name"; echo x > "$tmp/seed-name/bad name"
seed_case "the checker refuses an original file name the lock refuses" 1 "seed: file name 'bad name' must be" \
  "$tmp/seed-name"
requirements 1.10.1
seed_case "the checker refuses a seed its tools' lines do not give" 1 "$seed_differs" "$tmp/set"
requirements 1.10.0 ""
seed_case "the checker refuses a tool line with no SHA-256, which no seed covers" 1 \
  "'delvewheel==1.10.0' is not one <name>==<version> --hash=sha256:<hex> line per tool" "$tmp/set"
# A second line for one tool: the pin keeps the last of the two and refuses nothing, so only the
# checker's own parse can refuse it.
requirements 1.10.0
head -1 "$lib/rename-requirements.txt" >> "$lib/rename-requirements.txt"
seed_case "the checker refuses a second line for one tool, which the pin would take silently" 1 \
  "'delvewheel==1.10.0 --hash=sha256:$dw --url=$dw_url' is not one" "$tmp/set"
printf '# no tool\n' > "$lib/rename-requirements.txt"
seed_case "the checker refuses a requirements file that pins no tool" 1 "pins no tool" "$tmp/set"
requirements 1.10.0
# with_version <file> <line> <name> <exit> <text>: the case with <file>'s VERSION line replaced.
with_version() {
  cp "$lib/$1" "$tmp/$1.kept"
  python3 -I -c 'import re, sys
p, line = sys.argv[1], sys.argv[2]
text = open(p).read()
open(p, "w").write(re.sub(r"^VERSION = [0-9]+", line, text, count=1, flags=re.M))' "$lib/$1" "$2"
  seed_case "$3" "$4" "$5" "$tmp/set"
  cp "$tmp/$1.kept" "$lib/$1"
}
with_version rename_apply.py "VERSION = 2" "the checker refuses a seed of another applier version" 1 "$seed_differs"
with_version rename_pin.py "VERSION = 2" "the checker refuses a seed of another pin version" 1 "$seed_differs"
with_version rename_pin.py "VERSIONS = 1" "the checker refuses a pin without one VERSION line" 1 \
  "rename_pin.py does not hold one 'VERSION = <n>' line"
# Both lines give the pin's own version, so a checker reading only the first would pass it.
with_version rename_pin.py $'VERSION = 1\nVERSION = 1' "the checker refuses a pin with two VERSION lines" 1 \
  "rename_pin.py does not hold one 'VERSION = <n>' line"
seed_case "the restored tree's checker passes the applier's output again" 0 "" "$tmp/set"
# The pin's rows in a format 2 lock, with file rows of the lock's form around them, pass the lock's
# reader: the rename, patch, name-seed and set-hash rows are written as check-lock.sh reads them.
msi_url="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/2.3.0.0"
msi_url+="/AmazonRedshiftODBC64-2.3.0.0.msi"
notices="https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v2.3.0"
{
  printf '%s\n' "format 2" "version 2.3.0.0" "status development"
  awk -v u="$msi_url" -v m="$(printf 'e%.0s' {1..64})" '$2 == "driver" || $2 == "library" {
    print $1, $2, $3, $4, $5, u, m, "./PFiles64/Drivers/" $3 }' "$tmp/rows"
  awk '$2 == "rename" || $2 == "patch" || $2 == "name-seed" || $2 == "set-hash"' "$tmp/rows"
  for notice in LICENSE NOTICE THIRD_PARTY_LICENSES; do
    echo "*  notice  $notice  $(printf 'f%.0s' {1..64})  $notices/$notice"
  done
} > "$tmp/pin.lock"
expect "the pin's rows, in a format 2 lock, pass check-lock.sh" 0 "" bash \
  "$odbc/tools/check-lock.sh" "$tmp/pin.lock"
expect "the applier and the checker read that lock's file rows, which carry the MSI fields" 0 "" bash -c \
  'python3 -I "$1/rename_apply.py" "$2" "$3" "$4" && python3 -I "$1/rename_check.py" "$2" "$3" "$4"' \
  _ "$lib" "$tmp/pin.lock" "$tmp/set" "$tmp/lock-out"
ok=1
while read -r _ kind name sum _; do
  [[ "$kind" == driver || "$kind" == library ]] || continue
  generated="$(awk -v n="$name" '$2 == "rename" && $3 == n { print $4 }' "$tmp/rows")"
  [[ "$(sha "$tmp/out/$generated")" == "$sum" ]] || ok=0
done < "$tmp/rows"
g_msvcr="$(awk '$3 == "msvcr100.dll" && $2 == "rename" { print $4 }' "$tmp/rows")"
if ((ok)) && [[ "$(sha "$tmp/out/$g_msvcr")" == "$(sha "$tmp/set/msvcr100.dll")" ]]; then
  pass "each renamed file's SHA-256 is its row's, and msvcr100.dll's bytes are unchanged"
else fail "each renamed file's SHA-256 is its row's, and msvcr100.dll's bytes are unchanged"; fi
# Counted numerically: BSD wc pads its count with spaces.
rows_where() { awk "$1" "$tmp/rows" | wc -l; }
utf16_name=6c0065006100730068007700360034002e0064006c006c
if (($(rows_where '$2 == "patch" && $3 == "krb5_64.dll" && $5 ~ /'"$utf16_name"'/') == 1 &&
  $(rows_where '$2 == "patch" && $3 == "rsodbc.dll" && $4 == 152') == 1 &&
  $(rows_where '$2 == "patch" && $3 == "rsodbc.dll"') > 1)); then
  pass "a patch row joins changes fewer than 8 bytes apart, so a UTF-16LE name is one row, and no others"
else fail "a patch row joins changes fewer than 8 bytes apart, so a UTF-16LE name is one row, and no others"; fi
lengths="$(awk '$2 == "rename" { print $3, length($4) - 4 }' "$tmp/rows" | sort | tr '\n' ' ')"
if [[ "$lengths" == "crypto.dll 16 krb5_64.dll 7 msvcr100.dll 16 rsodbc.dll 16 " ]] &&
  ! awk '$2 == "rename" { print $4 }' "$tmp/rows" | grep -qv '^[a-z2-7]*\.dll$'; then
  pass "generated names are 16 characters from a-z2-7, krb5_64's its own 7, each with .dll"
else fail "generated names are 16 characters from a-z2-7, krb5_64's its own 7, each with .dll
  ($(grep rename "$tmp/rows"))"; fi
"${pin[@]}" pin "$tmp/set" rsodbc.dll > "$tmp/rows2"
if same "$tmp/rows" "$tmp/rows2"; then pass "two pin runs give the same rows"; else fail "two pin runs give the same rows"; fi
"${apply[@]}" "$tmp/rows" "$tmp/set" "$tmp/out2"
if same "$tmp/out" "$tmp/out2"; then pass "two applier runs give identical bytes"; else fail "two applier runs give identical bytes"; fi
cp -R "$tmp/out" "$tmp/embedded"; notices "$tmp/embedded"
manifest_sha="$(python3 -I -c 'import hashlib, os, sys
d = sys.argv[1]
m = "".join("%s  %d  %s\n" % (hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest(), os.path.getsize(os.path.join(d, f)), f) for f in sorted(os.listdir(d)))
print(hashlib.sha256(m.encode()).hexdigest())' "$tmp/embedded")"
if grep -q "set-hash  $manifest_sha" "$tmp/rows"; then
  pass "the set hash is the manifest's of the renamed set"
else fail "the set hash is the manifest's of the renamed set"; fi

# The names: derived from the name seed and the base name, so every original and the transform count.
names_of() { "${pin[@]}" names "$1" | awk -v n="$2" '$3 == n { print $4 }'; }
set_files "$tmp/other" "OpenSSL 4"
if same "$tmp/set/krb5_64.dll" "$tmp/other/krb5_64.dll" &&
  [[ "$(names_of "$tmp/set" krb5_64.dll)" != "$(names_of "$tmp/other" krb5_64.dll)" ]]; then
  pass "two sets sharing krb5_64.dll byte for byte, differing only in crypto.dll, give it different names"
else fail "two sets sharing krb5_64.dll byte for byte, differing only in crypto.dll, give it different names"; fi
if [[ "$(awk '$2 == "rename"' "$tmp/rows" | sort)" == "$("${pin[@]}" names "$tmp/set" | awk '$2 == "rename"' | sort)" ]]; then
  pass "names prints the names pin uses"; else fail "names prints the names pin uses"; fi
requirements 1.10.1
if [[ "$(names_of "$tmp/set" crypto.dll)" != "$(awk '$2 == "rename" && $3 == "crypto.dll" { print $4 }' "$tmp/rows")" ]]; then
  pass "the same originals under another delvewheel version get different names"
else fail "the same originals under another delvewheel version get different names"; fi
expect "pin refuses a cached wheel of another version than the pinned one" 1 "not the pinned 1.10.1" "${pin[@]}" pin "$tmp/set" rsodbc.dll
requirements 1.10.0
# Every requirements line is part of the transform, so another pefile renames every DLL too.
# kept_of: how many of $tmp/set's DLLs keep their name from $tmp/rows, and of how many.
kept_of() {
  "${pin[@]}" names "$tmp/set" | awk 'NR == FNR { if ($2 == "rename") old[$3] = $4; next }
    $2 == "rename" { total++; kept += old[$3] == $4 } END { print kept + 0, total + 0 }' "$tmp/rows" -
}
cp "$lib/rename-requirements.txt" "$tmp/requirements-pinned"
for bump in "version:s/^pefile==2024\.8\.26 /pefile==2024.8.27 /" "SHA-256:s/$pf/$(printf '0%.0s' {1..64})/"; do
  sed "${bump#*:}" "$tmp/requirements-pinned" > "$lib/rename-requirements.txt"
  kept="$(kept_of || :)"
  if [[ "$kept" == "0 4" ]]; then pass "a pefile of another ${bump%%:*} gives every DLL another name"
  else fail "a pefile of another ${bump%%:*} gives every DLL another name (kept, of: $kept)"; fi
done
cp "$tmp/requirements-pinned" "$lib/rename-requirements.txt"
set_files "$tmp/xp" "xpprof64"
before="$(names_of "$tmp/xp" krb5_64.dll)"
cp "$policy" "$tmp/policy-kept"; echo "xpprof64" >> "$policy"
after="$(names_of "$tmp/xp" krb5_64.dll)"
cp "$tmp/policy-kept" "$policy"
if [[ -n "$before" && "$before" != "$after" ]]; then pass "a string newly named by the unshipped-names policy changes every name"
else fail "a string newly named by the unshipped-names policy changes every name"; fi
mkdir -p "$tmp/case"; cp "$tmp/set"/* "$tmp/case/"; cp "$tmp/set/krb5_64.dll" "$tmp/case/KRB5_64.DLL"
# A file system that folds case (macOS's default) keeps one file for the two names, which collide
# only where both are kept.
folded=("$tmp/case"/[kK][rR][bB]5_64.[dD][lL][lL])
if ((${#folded[@]} == 2)); then
  expect "two DLLs whose names differ only in case collide and fail the pin" 1 "two generated names collide" "${pin[@]}" names "$tmp/case"
else
  n=$((n+1)); echo "ok $n - two DLLs whose names differ only in case collide and fail the pin # SKIP the file system folds case"
fi
set_files "$tmp/held" "crypto.dll"
expect "a string naming a 16-character member fails the pin" 1 "names: a set file holds the name crypto.dll" "${pin[@]}" names "$tmp/held"
mkdir "$tmp/unheld"; pe "$tmp/unheld/krb5_64.dll" '{"rdata": {"imports": ["KERNEL32.dll"]}}'
cp "$tmp/unheld/krb5_64.dll" "$tmp/unheld/wshelp64.dll"
lengths="$("${pin[@]}" names "$tmp/unheld" | awk '$2 == "rename" { sub(/\.dll$/, "", $4); printf "%s %d ", $3, length($4) }' || :)"
if [[ "$lengths" == "krb5_64.dll 7 wshelp64.dll 8 " ]]; then
  pass "pin gives krb5_64 7 characters and wshelp64 8, as the lock does, with no string naming them"
else fail "pin gives krb5_64 7 characters and wshelp64 8, as the lock does, with no string naming them ($lengths)"; fi
expect "a routine that makes a file longer fails the pin" 1 "pin: delvewheel changed krb5_64.dll's length" \
  env RS_FAKE_DELVEWHEEL_GROW=1 python3 -I "$lib/rename_pin.py" pin "$tmp/set" rsodbc.dll
# The pinned routine clears the driver's DependentLoadFlags (2304 here); the gate refuses that.
expect "the checker refuses a set whose routine cleared DependentLoadFlags" 1 "bytes: rsodbc.dll changed at" \
  bash -c 'env RS_FAKE_DELVEWHEEL_CLEAR_FLAGS=1 python3 -I "$1/rename_pin.py" pin "$2" rsodbc.dll > "$3" &&
    python3 -I "$1/rename_apply.py" "$3" "$2" "$4" && python3 -I "$1/rename_check.py" "$3" "$2" "$4"' \
  _ "$lib" "$tmp/set" "$tmp/cleared-rows" "$tmp/cleared-out"
expect "pin refuses a driver that is not in the set" 1 "the driver ssl.dll is not a .dll" "${pin[@]}" pin "$tmp/set" ssl.dll
mkdir "$tmp/nodll"; echo x > "$tmp/nodll/LICENSE"
expect "pin refuses a set with no .dll" 1 "holds no .dll" "${pin[@]}" names "$tmp/nodll"
mkdir -p "$tmp/subdir/x.dll"
expect "pin refuses a set entry that is not a regular file" 1 "is not a regular file" "${pin[@]}" names "$tmp/subdir"
mkdir "$tmp/badname"; cp "$tmp/set/crypto.dll" "$tmp/badname/-a.dll"
expect "pin refuses a set file name the lock refuses" 1 "set: file name '-a.dll'" "${pin[@]}" names "$tmp/badname"
# Sparse, so it takes no disk; the suite's temporary directory goes at exit.
mkdir "$tmp/huge"; python3 -I -c 'import sys; open(sys.argv[1], "wb").truncate((1 << 30) + 1)' "$tmp/huge/a.dll"
expect "pin refuses a set file larger than 1 GiB, unread" 1 "is larger than 1 GiB" "${pin[@]}" names "$tmp/huge"
mkdir "$tmp/long"; pe "$tmp/long/a.dll" "{\"rdata\": {\"imports\": [\"$(printf 'k%.0s' {1..300}).dll\"]}}"
expect "pin refuses an import name of more than 256 bytes" 1 "an import's DLL name has no NUL within 256 bytes" \
  "${pin[@]}" names "$tmp/long"
mkdir "$tmp/alone"
pe "$tmp/alone/krb5_64.dll" \
  '{"rdata": {"imports": ["KERNEL32.dll"], "strings": [["a", "KRB5_64"]]}}'
expect "pin needs no tool for a set whose DLLs import no set member" 0 "rename  krb5_64.dll" \
  env RS_RENAME_TOOLS="$tmp/empty" python3 -I "$lib/rename_pin.py" pin "$tmp/alone" krb5_64.dll
cp "$policy" "$tmp/policy-kept"; echo "KRBCC64" >> "$policy"
expect "the tools refuse a policy name that is not a lowercase base name" 1 "lists 'KRBCC64', not a lowercase base name" \
  "${pin[@]}" names "$tmp/set"
cp "$tmp/policy-kept" "$policy"
mkdir "$tmp/notpe"; echo text > "$tmp/notpe/a.dll"
expect "pin refuses a DLL that is not a PE image" 1 "image: not a PE image" "${pin[@]}" names "$tmp/notpe"
expect "pin exits 3, naming the tool, when the cache lacks it" 3 "delvewheel==1.10.0 is not in" \
  env RS_RENAME_TOOLS="$tmp/empty" python3 -I "$lib/rename_pin.py" pin "$tmp/set" rsodbc.dll
mkdir "$tmp/swapped"; cp "$tmp/tools/$pf.whl" "$tmp/swapped/$dw.whl"; cp "$tmp/tools/$pf.whl" "$tmp/swapped/$pf.whl"
expect "pin refuses a cached wheel whose bytes are not the pinned ones" 1 "is not the pinned delvewheel" \
  env RS_RENAME_TOOLS="$tmp/swapped" python3 -I "$lib/rename_pin.py" pin "$tmp/set" rsodbc.dll
expect "names needs no tool" 0 "rename  rsodbc.dll" env RS_RENAME_TOOLS="$tmp/empty" python3 -I "$lib/rename_pin.py" names "$tmp/set"
expect "names on a set folder that does not exist fails at the set rule, not with a traceback" 1 \
  "set: cannot list $tmp/absent: No such file or directory" python3 -I "$lib/rename_pin.py" names "$tmp/absent"
# A narrow literal followed directly by a wide one: the ASCII string's last character and NUL also
# read as a UTF-16 unit.
adjacent="$(python3 -I -c 'print((b"\0abc\0" + "leashw64.dll".encode("utf-16-le") + b"\0\0").hex())')"
mkdir "$tmp/adjacent"; cp "$tmp/set"/* "$tmp/adjacent/"
pe "$tmp/adjacent/crypto.dll" "{\"rdata\": {\"imports\": [\"KERNEL32.dll\"], \"strings\": [[\"h\", \"$adjacent\"]]}}"
expect "pin rewrites a UTF-16LE name right after an ASCII string's NUL, and the checker passes it" 0 \
  "none left" bash -c 'python3 -I "$1/rename_pin.py" pin "$2" rsodbc.dll > "$3" &&
    python3 -I "$1/rename_apply.py" "$3" "$2" "$4" && python3 -I "$1/rename_check.py" "$3" "$2" "$4" &&
    python3 -I -c "import os, sys
d = sys.argv[1]
sys.exit(any(\"leashw64\".encode(\"utf-16-le\") in open(os.path.join(d, f), \"rb\").read() for f in os.listdir(d)))" "$4" &&
    echo none left' _ "$lib" "$tmp/adjacent" "$tmp/adjacent-rows" "$tmp/adjacent-out"
mkdir "$tmp/hinted"
pe "$tmp/hinted/a.dll" '{"rdata": {"imports": ["KERNEL32.dll"], "imported_functions": ["leashw64"]}}'
expect "pin leaves an imported function's name unchanged, even an unshipped DLL's" 0 "" bash -c \
  'python3 -I "$1" pin "$2" a.dll > "$3" && ! grep -q "  patch  " "$3"' _ "$lib/rename_pin.py" \
  "$tmp/hinted" "$tmp/hinted-rows"

# A bare base name is a self-name only for krb5_64 and wshelp64; any other is left unchanged, and
# only where policy/windows/kept-names.txt keeps it for the file's SHA-256 and offset.
at_of() { # at_of <file> <text>: the offset of the whole ASCII string <text> in <file>
  python3 -I -c 'import sys
d = open(sys.argv[1], "rb").read()
print(d.index(b"\0" + sys.argv[2].encode() + b"\0") + 1)' "$1" "$2"
}
kept="$lib/../../policy/windows/kept-names.txt"; cp "$kept" "$tmp/kept-orig"
# keep <row>...: the checked-in list with the rows added.
keep() { cp "$tmp/kept-orig" "$kept"; printf '%s\n' "$@" >> "$kept"; }
set_files "$tmp/bare" "CRYPTO"
c_sha="$(sha "$tmp/bare/crypto.dll")"; c_at="$(at_of "$tmp/bare/crypto.dll" CRYPTO)"
c_row="crypto.dll  $c_sha  $c_at  CRYPTO"
needle="kept-names: crypto.dll holds 'CRYPTO' at $c_at"
expect "a bare member name no row keeps fails the pin" 1 \
  "$needle, the bare name of the set member crypto.dll" "${pin[@]}" names "$tmp/bare"
keep "$c_row  an error-library name, read by no load"
expect "a bare member name its row keeps passes the pin" 0 "" bash -c \
  'python3 -I "$1" pin "$2" rsodbc.dll > "$3"' _ "$lib/rename_pin.py" "$tmp/bare" "$tmp/bare-rows"
expect "the kept name's rows reproduce the set, which the checker passes" 0 "" bash -c \
  'python3 -I "$1/rename_apply.py" "$2" "$3" "$4" &&
   python3 -I "$1/rename_check.py" "$2" "$3" "$4"' \
  _ "$lib" "$tmp/bare-rows" "$tmp/bare" "$tmp/bare-out"
g_bare="$(awk '$2 == "rename" && $3 == "crypto.dll" { print $4 }' "$tmp/bare-rows")"
k_gen="$(awk '$2 == "rename" && $3 == "krb5_64.dll" { print $4 }' "$tmp/bare-rows")"
k_at="$(at_of "$tmp/bare/krb5_64.dll" krb5_64)"
name="the kept string is unchanged, crypto.dll gets 16 characters, krb5_64's self-name is renamed"
if [[ "${#g_bare}" == 20 ]] && python3 -I -c 'import sys
a, b, k = (open(p, "rb").read() for p in sys.argv[1:4])
at, k_at = int(sys.argv[4]), int(sys.argv[5])
kept = a[at:at + 6] == b[at:at + 6] == b"CRYPTO"
sys.exit(not (kept and k[k_at:k_at + 7] == sys.argv[6][:7].encode()))' \
  "$tmp/bare/crypto.dll" "$tmp/bare-out/$g_bare" "$tmp/bare-out/$k_gen" "$c_at" "$k_at" "$k_gen"
then pass "$name"; else fail "$name"; fi
cp "$tmp/kept-orig" "$kept"
expect "the checker refuses a bare member name no row keeps" 1 "$needle" \
  "${check[@]}" "$tmp/bare-rows" "$tmp/bare" "$tmp/bare-out"
# The renamed file with the kept string changed and its row's sha256 following it, so only the
# string differs.
cp -R "$tmp/bare-out" "$tmp/bare-changed"
printf 'X' | dd of="$tmp/bare-changed/$g_bare" bs=1 seek="$c_at" conv=notrunc 2>/dev/null
before="$(sha "$tmp/bare-out/$g_bare")"; after="$(sha "$tmp/bare-changed/$g_bare")"
sed "s/  $before  /  $after  /" "$tmp/bare-rows" > "$tmp/bare-rows-changed"
keep "$c_row  r"
expect "the checker refuses a kept string the rename changed" 1 \
  "bytes: crypto.dll changed at $c_at" \
  "${check[@]}" "$tmp/bare-rows-changed" "$tmp/bare" "$tmp/bare-changed"
# The first hex digit flipped, so the hash is stale whatever digits it holds.
stale_sha="$([[ "${c_sha:0:1}" == 0 ]] && echo 1 || echo 0)${c_sha:1}"
for stale in "hash:$stale_sha  $c_at  CRYPTO" "offset:$c_sha  $((c_at + 1))  CRYPTO" \
  "text:$c_sha  $c_at  crypto"; do
  keep "crypto.dll  ${stale#*:}  r"
  expect "a row with a stale ${stale%%:*} fails the pin" 1 "$needle" "${pin[@]}" names "$tmp/bare"
  expect "a row with a stale ${stale%%:*} fails the checker" 1 "$needle" \
    "${check[@]}" "$tmp/bare-rows" "$tmp/bare" "$tmp/bare-out"
done
k_sha="$(sha "$tmp/bare/krb5_64.dll")"
keep "$c_row  r" "krb5_64.dll  $k_sha  $k_at  krb5_64  r"
expect "a row that keeps no bare member name of its file fails" 1 \
  "kept-names: the row for krb5_64.dll at $k_at (krb5_64, SHA-256 $k_sha) keeps no" \
  "${pin[@]}" names "$tmp/bare"
expect "the checker refuses a row that keeps no bare member name of its file" 1 \
  "kept-names: the row for krb5_64.dll at $k_at (krb5_64, SHA-256 $k_sha) keeps no" \
  "${check[@]}" "$tmp/bare-rows" "$tmp/bare" "$tmp/bare-out"
keep "$c_row  r" "ssl.dll  $c_sha  $c_at  SSL  r"
expect "a row for a file the set does not hold is read past" 0 "rename  crypto.dll" \
  "${pin[@]}" names "$tmp/bare"
for bad in "no reason:$c_row" "a short hash:crypto.dll  ${c_sha:1}  $c_at  CRYPTO  r" \
  "a leading zero:crypto.dll  $c_sha  0$c_at  CRYPTO  r" \
  "a bad file name:-a.dll  $c_sha  $c_at  CRYPTO  r" \
  "an upper-case file name:Crypto.dll  $c_sha  $c_at  CRYPTO  r"; do
  keep "${bad#*:}"
  expect "the tools refuse a kept-names row with ${bad%%:*}" 1 \
    "is not <file> <sha256> <offset> <text> <reason>" "${pin[@]}" names "$tmp/bare"
done
keep "$c_row  r" "$c_row  again"
expect "the tools refuse two rows for one file and offset" 1 "once per file and offset" \
  "${pin[@]}" names "$tmp/bare"
cp "$tmp/kept-orig" "$kept"

# The applier: checks before and after, and leaves nothing behind on a failure.
mkdir "$tmp/changed"; cp "$tmp/set"/* "$tmp/changed/"; printf 'X' | dd of="$tmp/changed/crypto.dll" bs=1 seek=600 conv=notrunc 2>/dev/null
expect "a changed original byte fails the applier" 1 "rename: crypto.dll's SHA-256 is" "${apply[@]}" "$tmp/rows" "$tmp/changed" "$tmp/o1"
# The first patch row's original bytes with their first hex digit changed, the hashes kept.
awk '$2 == "patch" && !done { $5 = (substr($5, 1, 1) == "f" ? "e" : "f") substr($5, 2); done = 1; p = $3 " at " $4 } { print }
  END { print p > "/dev/stderr" }' "$tmp/rows" > "$tmp/rows-orig" 2> "$tmp/patched"
expect "a patch whose original bytes differ from the file fails" 1 "patch: $(cat "$tmp/patched") does not hold the recorded original" \
  "${apply[@]}" "$tmp/rows-orig" "$tmp/set" "$tmp/o2"
sed "s/^\(windows_amd64  [a-z]*  crypto.dll  \)[0-9a-f]\{64\}/\1$(printf '0%.0s' {1..64})/" "$tmp/rows" > "$tmp/rows-out"
expect "a changed output hash fails the applier" 1 "rename: crypto.dll's renamed SHA-256 is" "${apply[@]}" "$tmp/rows-out" "$tmp/set" "$tmp/o3"
expect "the applier refuses an output directory that exists" 1 "cannot create" "${apply[@]}" "$tmp/rows" "$tmp/set" "$tmp/out"
awk '$2 == "rename" && $3 == "rsodbc.dll" { g = $4 } { r[NR] = $0 } END { for (i = 1; i <= NR; i++) {
  split(r[i], f); if (f[2] == "rename" && f[3] == "crypto.dll") print f[1], f[2], f[3], g; else print r[i] } }' "$tmp/rows" > "$tmp/rows-dup"
expect "two files renamed to one name fail and leave no output directory" 1 "cannot write into" bash -c \
  'python3 -I "$1" "$2" "$3" "$4"; status=$?; [[ -e $4 ]] && exit 9; exit $status' _ "$lib/rename_apply.py" "$tmp/rows-dup" "$tmp/set" "$tmp/o4"
sum="$(printf 'a%.0s' {1..64})"
# rows <name> <output-substring> <rows after a.dll's library row>: the applier fails reading them.
rows() {
  printf 'format 2\nwindows_amd64  library  a.dll  %s  %s\n%s\n' "$sum" "$sum" "$3" > "$tmp/r"
  expect "$1" 1 "$2" "${apply[@]}" "$tmp/r" "$tmp/set" "$tmp/o5"
}
rows "rows refuse uppercase hex in a patch" "line 3: patch a.dll takes" 'windows_amd64  patch  a.dll  10  AB  cd'
rows "rows refuse a hex string of odd length" "line 3: patch a.dll takes" 'windows_amd64  patch  a.dll  10  abc  cde'
rows "rows refuse an offset with a leading zero" "line 3: patch a.dll takes" 'windows_amd64  patch  a.dll  010  ab  cd'
rows "rows refuse an offset in hex" "line 3: patch a.dll takes" 'windows_amd64  patch  a.dll  0x10  ab  cd'
rows "rows refuse byte strings of two lengths" "changes 1 bytes into 2" 'windows_amd64  patch  a.dll  10  ab  cdef'
rows "rows refuse a patch that overlaps the one before" "overlaps or precedes" \
  "$(printf 'windows_amd64  patch  a.dll  10  abab  cdcd\nwindows_amd64  patch  a.dll  11  ab  cd')"
for bad in abcdefghijklmno0 abcdefghijklmno1 abcdefghijklmno8 abcdefghijklmno9 Abcdefghijklmnop abcdefghijklmn_p abcdef abcdefghijklmnopq; do
  rows "rows refuse the generated name $bad" "generated name '$bad.dll' is not 16, 7 or 8" \
    "windows_amd64  rename  a.dll  $bad.dll"
done
# The lock's one spelling: the generated name with .dll, so a bare or otherwise suffixed name fails.
for bad in abcdefghijklmnop abcdefghijklmnop.DLL abcdefghijklmnop.exe abcdefghijklmnop.dll.dll; do
  rows "rows refuse the generated name written $bad" "generated name '$bad' is not 16, 7 or 8" \
    "windows_amd64  rename  a.dll  $bad"
done
expect "rows refuse a hash of 63 characters" 1 "takes a sha256 and a source-sha256" \
  bash -c 'printf "windows_amd64  driver  a.dll  %s  %s\n" "${1:1}" "$1" > "$2"; python3 -I "$3" "$2" "$4" "$5"' _ "$sum" "$tmp/r" "$lib/rename_apply.py" "$tmp/set" "$tmp/o6"
rows "rows refuse a row with no file name" "rename row has no file name" 'windows_amd64  rename'
rows "rows refuse a second row for one file" "takes a sha256 and a source-sha256" "windows_amd64  library  a.dll  $sum  $sum"
rows "rows refuse a file row with only part of the MSI fields" "takes a sha256 and a source-sha256" \
  "windows_amd64  library  b.dll  $sum  $sum  $msi_url"
rows "rows refuse a rename to two names" "rename a.dll takes one generated name, once" \
  'windows_amd64  rename  a.dll  abcdefg.dll  hijklmn.dll'
rows "rows refuse an unknown kind" "unknown row kind 'renamed'" \
  'windows_amd64  renamed  a.dll  abcdefg.dll'
rows "rows refuse a file name the lock refuses" "file name '-a.dll'" \
  'windows_amd64  rename  -a.dll  abcdefg.dll'
rows "rows refuse a second name seed" "name-seed takes one SHA-256" \
  "$(printf 'windows_amd64  name-seed  %s\nwindows_amd64  name-seed  %s' "$sum" "$sum")"
rows "rows refuse a rename of a file with no driver or library row" "b.dll is renamed or patched but has no" \
  'windows_amd64  rename  b.dll  abcdefg.dll'
rows "rows read past a prerequisite row and another platform's rows" "cannot read $tmp/set/a.dll" \
  "$(printf '%s\n' 'windows_amd64  prerequisite  VCRUNTIME140.dll  14.0.0.0' \
     'linux_amd64  driver  x' 'windows_amd64  rename  a.dll  abcdefg.dll')"

# The checker, on one file x.dll and the set member crypto.dll, which imports only KERNEL32 and so
# is renamed unchanged, with the notices. pair <name> <exit> <text> <original spec> <renamed spec>
# [<x's generated name>]
# The rows' name seed is the one the pin's names step gives the originals, which the checker
# recomputes, and the generated names derive from it, so @gc@ and @gk@ in the renamed spec and in
# <x's generated name> stand for crypto's and krb5_64's; after a pair, gx, gc and gk are its names.
# Where the pin refuses the originals (a bare member name no row keeps), the fixed seed below
# stands in, as the checker fails before it recomputes one; fixed=1 uses it whatever the pin says.
seed="$(printf 'd%.0s' {1..64})"
derive() { # derive <base name> <length>: the generated name the seed $dseed, else $seed, gives it
  python3 -I -c 'import base64, hashlib, sys
d = hashlib.sha256((sys.argv[1] + " " + sys.argv[2]).encode()).digest()
print(base64.b32encode(d).decode().lower()[:int(sys.argv[3])])' "${dseed:-$seed}" "$1" "$2"
}
gx="$(derive x 16)"; gc="$(derive crypto 16)"; gk="$(derive krb5_64 7)"
pe "$tmp/crypto.dll" '{"rdata": {"imports": ["KERNEL32.dll"]}}'
pair() { # px names the file when set, x.dll otherwise
  local x="${px:-x.dll}" spec name
  rm -rf -- "${tmp:?}/p"; mkdir -p "$tmp/p/o" "$tmp/p/r"
  pe "$tmp/p/o/$x" "$4"; cp "$tmp/crypto.dll" "$tmp/p/o/"; notices "$tmp/p/o"
  dseed=""
  [[ "${fixed:-}" == 1 ]] ||
    dseed="$("${pin[@]}" names "$tmp/p/o" 2>/dev/null | awk '$2 == "name-seed" { print $3 }')" || dseed=""
  dseed="${dseed:-$seed}"
  gx="$(derive x 16)"; gc="$(derive crypto 16)"; gk="$(derive krb5_64 7)"
  spec="${5//@gc@/$gc}"; spec="${spec//@gk@/$gk}"; name="${6:-$gx}"; name="${name//@gk@/$gk}"
  pe "$tmp/p/r/$name.dll" "$spec"; cp "$tmp/crypto.dll" "$tmp/p/r/$gc.dll"
  printf 'windows_amd64  library  %s  %s  %s\nwindows_amd64  rename  %s  %s.dll\n' \
    "$x" "$(sha "$tmp/p/r/$name.dll")" "$(sha "$tmp/p/o/$x")" "$x" "$name" > "$tmp/p/rows"
  printf 'windows_amd64  library  crypto.dll  %s  %s\nwindows_amd64  rename  crypto.dll  %s.dll\n' \
    "$(sha "$tmp/crypto.dll")" "$(sha "$tmp/crypto.dll")" "$gc" >> "$tmp/p/rows"
  printf 'windows_amd64  name-seed  %s\n' "$dseed" >> "$tmp/p/rows"
  expect "$1" "$2" "$3" "${check[@]}" "$tmp/p/rows" "$tmp/p/o" "$tmp/p/r"
}
fixed=1 pair "the checker refuses rows whose names derive from a seed other than the one it computes" 1 \
  "seed: the name-seed row $seed is not" '{"rdata": {}}' '{"rdata": {}}'
o='{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"]}, "checksum": "valid"}'
r='{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"], "renamed": {"crypto.dll": "@gc@.dll"}}, "checksum": "valid"}'
pair "the checker passes an import renamed into the section's padding" 0 "" "$o" "$r"
# The same pair with one descriptor's TimeDateStamp changed: only its Name field may change.
python3 -I - "$lib" "$tmp/p/r/$gx.dll" <<'PY'
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("pe", sys.argv[1] + "/pe.py")
pe = importlib.util.module_from_spec(spec); spec.loader.exec_module(pe)
data = bytearray(open(sys.argv[2], "rb").read())
struct.pack_into("<I", data, pe.imports(pe.Image(bytes(data)))[1].descriptor + 4, 1)
open(sys.argv[2], "wb").write(data)
PY
awk -v s="$(sha "$tmp/p/r/$gx.dll")" '$3 == "x.dll" && $2 == "library" { $4 = s } { print }' "$tmp/p/rows" > "$tmp/p/rows-ts"
expect "the checker refuses an import descriptor whose TimeDateStamp changed" 1 "bytes: x.dll changed at" \
  "${check[@]}" "$tmp/p/rows-ts" "$tmp/p/o" "$tmp/p/r"
pair "the checker refuses an import renamed to another name" 1 "imports: x.dll imports kernel32.dll zzzz" "$o" \
  '{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"], "renamed": {"crypto.dll": "zzzzzzzzzzzzzzzz.dll"}}, "checksum": "valid"}'
pair "the checker refuses an import name past its section's virtual size" 1 "lies past its section's virtual size" "$o" \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}, \"virtual_size\": 150}, \"checksum\": \"valid\"}"
pair "the checker refuses a new import name written over other bytes" 1 "bytes: x.dll changed at" \
  '{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"], "strings": [["a", "zzzzzzzzzzzzzzzzzzzzzzzz"]]}}' \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"strings\": [[\"a\", \"zzzzzzzzzzzzzzzzzzzzzzzz\"]], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}, \"renamed_over\": 0}}"
# Zero bytes inside the section's content: the routine writes a moved name only past it.
zeros="$(printf '00%.0s' {1..24})"
pair "the checker refuses a new import name written over zero bytes inside the section's content" 1 \
  "bytes: x.dll changed at" \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"strings\": [[\"h\", \"$zeros\"]]}}" \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"strings\": [[\"h\", \"$zeros\"]], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}, \"renamed_over\": 0}}"
pair "the checker refuses a section whose virtual size shrinks" 1 "sections: x.dll's section at 0x8000" \
  '{"rdata": {}}' '{"rdata": {"virtual_size": 10}}'
pair "the checker recomputes the checksum of a file of odd length" 0 "" \
  '{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"]}, "checksum": "valid", "certificate": "x"}' \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}}, \"checksum\": \"valid\", \"certificate\": \"x\"}"
pair "the checker reads a plain export as no forwarder" 0 "" \
  '{"rdata": {"export_name": "crypto.dll", "functions": 1}}' '{"rdata": {"export_name": "crypto.dll", "functions": 1}}'
pair "the checker reads no DependentLoadFlags from a load configuration too short to hold them" 0 "" \
  '{"rdata": {"load_flags": 2048, "load_config_size": 64}}' '{"rdata": {"load_flags": 2048, "load_config_size": 64}}'
pair "the checker refuses a section growing past its raw size" 1 "sections: x.dll's section at 0x8000" "$o" \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}, \"virtual_size\": 1025}, \"checksum\": \"valid\"}"
pair "the checker refuses a section given no new import name growing" 1 "sections: x.dll's section at 0x1000" \
  '{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll"]}, "resources": [[10, 1, 0, "x"]], "checksum": "valid"}' \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}}, \"resources\": [[10, 1, 0, \"x\"]], \"virtual_size\": 200, \"checksum\": \"valid\"}"
grown="$(pe /dev/stdout "${r//@gc@/$gc}" | python3 -I -c 'import importlib.util, sys
spec = importlib.util.spec_from_file_location("pe", sys.argv[1] + "/pe.py")
pe = importlib.util.module_from_spec(spec); spec.loader.exec_module(pe)
print(pe.Image(sys.stdin.buffer.read()).sections[-1].virtual_size)' "$lib")"
pair "the checker refuses a section grown past its new import name's end" 1 "sections: x.dll's section at 0x8000" "$o" \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}, \"virtual_size\": $((grown + 2))}, \"checksum\": \"valid\"}"
pair "the checker refuses a changed nonzero checksum not recomputed" 1 "checksum: x.dll's checksum" "$o" \
  "{\"rdata\": {\"imports\": [\"KERNEL32.dll\", \"crypto.dll\"], \"renamed\": {\"crypto.dll\": \"@gc@.dll\"}}, \"checksum\": $(python3 -I "$here/make_pe.py" /dev/stdout "$o" | python3 -I -c 'import struct,sys; print(struct.unpack_from("<I", sys.stdin.buffer.read(), 0x98)[0])')}"
pair "the checker refuses a zero checksum made nonzero" 1 "checksum: x.dll's checksum was 0" \
  '{"rdata": {"imports": ["KERNEL32.dll"]}}' '{"rdata": {"imports": ["KERNEL32.dll"]}, "checksum": "valid"}'
pair "the checker refuses a byte changed elsewhere" 1 "bytes: x.dll changed at" \
  '{"rdata": {"strings": [["a", "abcdefg"]]}}' '{"rdata": {"strings": [["a", "abcdefh"]]}}'
pair "the checker refuses a file of another length" 1 "bytes: x.dll's renamed file is" \
  '{"rdata": {}}' '{"rdata": {}, "trailing": "x"}'
pair "the checker refuses a leftover original name in ASCII" 1 "the name CRYPTO.dll became CRYPTO.dll, not its generated name" \
  '{"rdata": {"strings": [["a", "CRYPTO.dll"]]}}' '{"rdata": {"strings": [["a", "CRYPTO.dll"]]}}'
pair "the checker refuses a leftover original name in UTF-16LE" 1 \
  "the name crypto.dll became crypto.dll, not its generated name" \
  '{"rdata": {"strings": [["u", "crypto.dll"]]}}' '{"rdata": {"strings": [["u", "crypto.dll"]]}}'
pair "the checker refuses a bare member name in UTF-16LE that no row keeps" 1 \
  "kept-names: x.dll holds 'crypto'" \
  '{"rdata": {"strings": [["u", "crypto"]]}}' '{"rdata": {"strings": [["u", "crypto"]]}}'
px=krb5_64.dll pair "the checker passes a self-name rewritten in place" 0 "" \
  '{"rdata": {"strings": [["a", "KRB5_64"], ["u", "krb5_64.dll"]]}}' \
  '{"rdata": {"strings": [["a", "@gk@"], ["u", "@gk@.dll"]]}}' @gk@
pair "the checker passes an unshipped name rewritten to a string no file can have" 0 "" \
  '{"rdata": {"strings": [["u", "leashw64"]]}}' '{"rdata": {"strings": [["u", "||||||||"]]}}'
pair "the checker refuses an unshipped name rewritten to a file name" 1 "the unshipped name leashw64 became leashw65" \
  '{"rdata": {"strings": [["u", "leashw64"]]}}' '{"rdata": {"strings": [["u", "leashw65"]]}}'
pair "the checker refuses an unshipped name rewritten to a path a load can open" 1 \
  "the unshipped name leashw64 became c:/x/a.d, not 8 '|' characters" \
  '{"rdata": {"strings": [["u", "leashw64"]]}}' '{"rdata": {"strings": [["u", "c:/x/a.d"]]}}'
pair "the checker refuses a UTF-16LE unshipped name right after an ASCII string, left unchanged" 1 \
  "the unshipped name leashw64.dll became leashw64.dll" \
  "{\"rdata\": {\"strings\": [[\"h\", \"$adjacent\"]]}}" "{\"rdata\": {\"strings\": [[\"h\", \"$adjacent\"]]}}"
# The search goes on past an allowed occurrence: here the version resource, before .rdata.
pair "the checker's own search refuses a name after one in an allowed place" 1 \
  "still holds leashw64, where no DLL may name one" \
  '{"rdata": {"strings": [["a", "C:\\MIT\\LEASHW64.DLL"]]}, "resources": [[16, 1, 0, "leashw64.dll"]]}' \
  '{"rdata": {"strings": [["a", "C:\\MIT\\LEASHW64.DLL"]]}, "resources": [[16, 1, 0, "leashw64.dll"]]}'
# A self-name is searched for bare as well, after a printable byte where the pin's finder reads none.
self_after_byte="$(python3 -I -c 'print((b"Z" + "krb5_64".encode("utf-16-le") + b"\0\0").hex())')"
px=krb5_64.dll pair "the checker's own search refuses a bare self-name the pin's finder does not see" 1 \
  "still holds krb5_64, where no DLL may name one" "{\"rdata\": {\"strings\": [[\"h\", \"$self_after_byte\"]]}}" \
  "{\"rdata\": {\"strings\": [[\"h\", \"$self_after_byte\"]]}}" @gk@
# Names the pin's whole-string finder does not see: a UTF-16LE one after a printable byte, which
# it reads as an ASCII string's end, and one at the end of a path.
after_byte="$(python3 -I -c 'print((b"Z" + "leashw64.dll".encode("utf-16-le") + b"\0\0").hex())')"
pair "the checker's own search refuses a UTF-16LE unshipped name the pin's finder does not see" 1 \
  "still holds leashw64" "{\"rdata\": {\"strings\": [[\"h\", \"$after_byte\"]]}}" \
  "{\"rdata\": {\"strings\": [[\"h\", \"$after_byte\"]]}}"
member_after_byte="$(python3 -I -c 'print((b"Z" + "crypto.dll".encode("utf-16-le") + b"\0\0").hex())')"
pair "the checker's own search refuses a UTF-16LE member name the pin's finder does not see" 1 \
  "still holds crypto.dll, where no DLL may name one" "{\"rdata\": {\"strings\": [[\"h\", \"$member_after_byte\"]]}}" \
  "{\"rdata\": {\"strings\": [[\"h\", \"$member_after_byte\"]]}}"
pair "the checker's own search refuses an unshipped name at the end of a path" 1 \
  "still holds leashw64, where no DLL may name one" '{"rdata": {"strings": [["a", "C:\\MIT\\LEASHW64.DLL"]]}}' \
  '{"rdata": {"strings": [["a", "C:\\MIT\\LEASHW64.DLL"]]}}'
pair "the checker passes an imported function and a PDB path that hold an unshipped DLL's name" 0 "" \
  '{"rdata": {"imports": ["KERNEL32.dll"], "imported_functions": ["leashw64"], "pdb": "C:\\b\\leashw64.pdb"}}' \
  '{"rdata": {"imports": ["KERNEL32.dll"], "imported_functions": ["leashw64"], "pdb": "C:\\b\\leashw64.pdb"}}'
pair "the checker passes the export name, version resource and certificate naming the file" 0 "" \
  '{"rdata": {"export_name": "x.dll"}, "resources": [[16, 1, 0, "x.dll"]], "certificate": "\u0000crypto.dll\u0000"}' \
  '{"rdata": {"export_name": "x.dll"}, "resources": [[16, 1, 0, "x.dll"]], "certificate": "\u0000crypto.dll\u0000"}'
if python3 -I -c 'import sys; sys.exit(not open(sys.argv[1], "rb").read().endswith(b"\0crypto.dll\0"))' "$tmp/p/o/x.dll"; then
  pass "that image ends with its certificate table, which names crypto.dll"
else fail "that image ends with its certificate table, which names crypto.dll"; fi
pair "the checker refuses a delay import naming a set member" 1 "x.dll delay-loads the set member crypto.dll" \
  '{"rdata": {"delay_imports": ["crypto.dll"]}}' '{"rdata": {"delay_imports": ["crypto.dll"]}}'
pair "the checker refuses a forwarder naming a set member" 1 "x.dll forwards CRYPTO.f to a set member" \
  '{"rdata": {"export_name": "y.dll", "forwarders": ["CRYPTO.f"]}}' '{"rdata": {"export_name": "y.dll", "forwarders": ["CRYPTO.f"]}}'
pair "the checker refuses a bound import" 1 "x.dll has bound imports" '{"rdata": {"bound_import": true}}' '{"rdata": {"bound_import": true}}'
pair "the checker passes a delay import and a forwarder naming system DLLs" 0 "" \
  '{"rdata": {"delay_imports": ["Secur32.dll"], "export_name": "y.dll", "forwarders": ["NTDLL.f"]}}' \
  '{"rdata": {"delay_imports": ["Secur32.dll"], "export_name": "y.dll", "forwarders": ["NTDLL.f"]}}'
for flags in 2048 768; do
  pair "the checker refuses DependentLoadFlags $flags" 1 "load-flags: x.dll's DependentLoadFlags" \
    "{\"rdata\": {\"load_flags\": $flags}}" "{\"rdata\": {\"load_flags\": $flags}}"
done
# Both values pass the load-flags rule, so only the bytes rule sees System32 dropped.
pair "the checker refuses DependentLoadFlags changed by the rename" 1 "bytes: x.dll changed at" \
  '{"rdata": {"load_flags": 2304}}' '{"rdata": {"load_flags": 256}}'
expect "the checker refuses a generated name that does not derive from the name seed" 1 \
  "rename: x.dll's generated name $gx does not derive from the name-seed row" \
  bash -c 'sed "s/name-seed  .*/name-seed  $(printf "e%.0s" {1..64})/" "$1" > "$1.4"
    python3 -I "$2" "$1.4" "$3" "$4"' \
  _ "$tmp/p/rows" "$lib/rename_check.py" "$tmp/p/o" "$tmp/p/r"
expect "the checker refuses rows without a name seed" 1 "rename: the rows have no name-seed row" \
  bash -c 'grep -v name-seed "$1" > "$1.5"; python3 -I "$2" "$1.5" "$3" "$4"' _ "$tmp/p/rows" \
    "$lib/rename_check.py" \
  "$tmp/p/o" "$tmp/p/r"
expect "the checker refuses a DLL with no rename row" 1 "rename: crypto.dll has no rename row" \
  bash -c 'grep -v "rename  crypto" "$1" > "$1.2"; python3 -I "$2" "$1.2" "$3" "$4"' _ "$tmp/p/rows" "$lib/rename_check.py" "$tmp/p/o" "$tmp/p/r"
expect "the checker refuses two files with one generated name" 1 "two files have one generated name" \
  bash -c 'sed "s/  $5\$/  $6/" "$1" > "$1.3"; python3 -I "$2" "$1.3" "$3" "$4"' _ "$tmp/p/rows" \
  "$lib/rename_check.py" "$tmp/p/o" "$tmp/p/r" "$gc.dll" "$gx.dll"
pair "the checker refuses a generated name that is not 16 characters" 1 \
  "rename: x.dll's generated name abcdefgh is not 16 characters" '{"rdata": {}}' '{"rdata": {}}' abcdefgh
gm="$(derive comerr64 8)"
px=comerr64.dll pair "the checker refuses a short generated name, from the seed, for a DLL that is not self-named" 1 \
  "rename: comerr64.dll's generated name $gm is not 16 characters" '{"rdata": {}}' '{"rdata": {}}' "$gm"
gk16="$(derive krb5_64 16)"
px=krb5_64.dll pair "the checker refuses a 16-character generated name for a self-named DLL" 1 \
  "rename: krb5_64.dll's generated name $gk16 is not 7 characters" '{"rdata": {}}' '{"rdata": {}}' "$gk16"
expect "the checker refuses files whose SHA-256s are not the rows'" 1 "original or renamed SHA-256 is not" \
  "${check[@]}" "$tmp/rows" "$tmp/changed" "$tmp/out"
expect "the checker refuses a renamed file whose SHA-256 is not its row's" 1 "original or renamed SHA-256 is not" \
  "${check[@]}" "$tmp/rows-out" "$tmp/set" "$tmp/out"

# The checked-in tools with the pinned delvewheel and pefile, on the same set but for the driver's
# DependentLoadFlags, which delvewheel clears and the checker refuses to see changed; the 2.3.0.0
# DLLs have none.
cp -R "$tmp/set" "$tmp/real-set"
pe "$tmp/real-set/rsodbc.dll" '{"rdata": {"imports": ["KERNEL32.dll", "crypto.dll", "KRB5_64.dll"],
  "export_name": "rsodbc.dll"}, "checksum": "valid"}'
if bounded env RS_RENAME_TOOLS="$real_tools" python3 -I "$odbc/tools/lib/rename_pin.py" pin \
  "$tmp/real-set" rsodbc.dll > "$tmp/real-rows" 2> "$tmp/real-err"; then
  expect "the pinned delvewheel's rows reproduce the set, which the checker passes" 0 "" bash -c \
    'python3 -I "$1/rename_apply.py" "$2" "$3" "$4" && python3 -I "$1/rename_check.py" "$2" "$3" "$4"' \
    _ "$odbc/tools/lib" "$tmp/real-rows" "$tmp/real-set" "$tmp/real-out"
elif [[ "$?" == 3 && "${RS_REQUIRE_RENAME_TOOLS:-}" != 1 ]]; then
  n=$((n+1)); echo "ok $n - the pinned delvewheel's rows reproduce the set # SKIP $(cat "$tmp/real-err")"
else fail "the pinned delvewheel's rows reproduce the set ($(cat "$tmp/real-err"))"; fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
