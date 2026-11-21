#!/usr/bin/env bash
# Tool tests for macos-hosts.sh's helpers, on any host, with inputs written here: macos_bindings.py, on tiny
# libraries make_macho.py writes, gives each weak bind's slot as the weak-bind opcodes place it, through each opcode
# it reads, and, from what the reader wrote, says which names went outside the driver and where, type information
# apart, and fails, saying so, where the host loaded no reader, the stream holds an opcode it does not read, a
# segment command is shorter than its fields, a bind names a segment the slice lacks, a symbol name runs past the
# stream or the stream lies outside the slice; macos_config_files.py, on fs_usage traces, passes a trace of the
# driver's own amazon.redshiftodbc.ini, a file of each kind of system file it exempts and the test host's own, reads
# /private/etc, /private/tmp and /private/var as the rows' /etc, /tmp and /var and a data-volume path as the path it
# backs, and fails one with a path no row lists (under HOME, in the current directory and under /System/Volumes/
# too), naming it, one that meets another row, one without the control path, and one without the driver's own
# amazon.redshiftodbc.ini, in this host's layout and in a Mac's (/private/var/folders/...), and refuses a wrong
# argument count; and macos-hosts.sh refuses a wrong argument count, a host that is not macOS, and a build it cannot
# run on, for each setting it needs, and, on stand-ins for macOS's tools, skips a host row with no host, or fails it
# with REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1, fails one whose host lipo cannot read, and tells the three signing
# states apart. The rows themselves run on macOS (make test_driver_e2e). Run by run-tool-tests.sh.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=bounded.sh
source "$here/bounded.sh" # bounded <command>...: at most 20 seconds, its process group stopped
policy="$here/../../../third_party/redshift_odbc/policy/config-files.txt"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
n=0 failed=0
check() { # check <name> <command...>
  local name="$1"
  shift
  n=$((n + 1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed + 1)); fi
}
# run <want status> <out file> <command...>: the command's output in <out file>, and its status is <want status>.
run() {
  local want="$1" out="$2" status=0
  shift 2
  "$@" > "$out" 2>&1 || status=$?
  [[ "$status" == "$want" ]] || { echo "# exit $status, not $want: $(cat "$out")"; return 1; }
}
has() { grep -qF -- "$2" "$1" || { echo "# no '$2' in: $(cat "$1")"; return 1; }; }
hasline() { grep -qxF -- "$2" "$1" || { echo "# no line '$2' in: $(cat "$1")"; return 1; }; }

# A driver whose slices weak-bind __Znwm and _redshift_own, one slot after the other, in __TEXT (segment 0, at
# vmaddr 0), laid out as in the weak-bind test: the driver's directory is in the one that holds the host's own files.
driver="$tmp/host/d/librsodbc64.dylib"
mkdir -p "$(dirname "$driver")" "$tmp/home" "$tmp/cwd"
python3 -I "$here/make_macho.py" "$driver" \
  '{"weak_bind_hex": "405f5f5a6e776d0090405f72656473686966745f6f776e0090"}'
check "slots: each weak bind's slot, by its offset from the header, and its name" \
  run 0 "$tmp/s.out" python3 -I "$here/macos_bindings.py" slots "$driver" arm64
check "  ...one after the other, as the opcodes bind them" test "$(cat "$tmp/s.out")" = "$(printf '0 __Znwm\n8 _redshift_own')"
# What weak_slots.c writes: each name and the image its slot's address is in.
printf '__Znwm weak_host\n_redshift_own librsodbc64.dylib\n_not_weak_bound libc++.1.dylib\n' > "$tmp/reached"
check "summary: the weak-bound names read, and where those outside the driver went" \
  run 0 "$tmp/b.out" python3 -I "$here/macos_bindings.py" summary "$driver" x86_64 "$tmp/reached" __Znwm _redshift_own
check "  ...counting only the driver's weak-bound names" \
  has "$tmp/b.out" "2 weak-bound names, 2 read; outside the driver: weak_host 1"
check "  ...each image's names, type information counted apart" has "$tmp/b.out" "  weak_host: 0 type information or type names; __Znwm"
check "  ...and naming where each given name went" has "$tmp/b.out" "__Znwm -> weak_host"
check "  ...the driver's own definition too" has "$tmp/b.out" "_redshift_own -> librsodbc64.dylib"
check "a host that did not load the reader fails the summary, saying so" \
  run 1 "$tmp/n.out" python3 -I "$here/macos_bindings.py" summary "$driver" arm64 "$tmp/none"
check "  ...with the reason" has "$tmp/n.out" "the host did not load the reader"

# The other opcodes, in __LINKEDIT (segment 1, at vmaddr 0x4000): a segment and offset (0x71, 0x10), an addend and
# a library ordinal skipped (0x60, 0x20), an added offset (0x80, 8), a bind that adds a ULEB (0xa0, 0x10), one
# that adds a scaled count (0xb2), two binds a ULEB apart (0xc0, 2, 8), then done, after which nothing is read.
linkedit="$tmp/linkedit/librsodbc64.dylib"
mkdir -p "$(dirname "$linkedit")"
python3 -I "$here/make_macho.py" "$linkedit" '{"weak_bind_hex": "405f5f5a54493141007110607f20028008a010405f5f5a5453314100b2405f6f746865725f7765616b00c0020800405f61667465725f646f6e650090"}'
check "slots: each opcode's slot, in its segment" \
  run 0 "$tmp/l.out" python3 -I "$here/macos_bindings.py" slots "$linkedit" x86_64
check "  ...and nothing after the done opcode" \
  test "$(cat "$tmp/l.out")" = "$(printf '4018 __ZTI1A\n4030 __ZTS1A\n4048 _other_weak\n4058 _other_weak')"
printf '__ZTI1A libc++abi.dylib\n__ZTS1A libc++abi.dylib\n' > "$tmp/rtti"
check "summary: type information and type names, counted apart, and a given name not read" \
  run 0 "$tmp/r.out" python3 -I "$here/macos_bindings.py" summary "$linkedit" x86_64 "$tmp/rtti" __ZTS1A _other_weak
check "  ...the count" hasline "$tmp/r.out" "3 weak-bound names, 2 read; outside the driver: libc++abi.dylib 2"
check "  ...both kinds as type information" hasline "$tmp/r.out" "  libc++abi.dylib: 2 type information or type names"
check "  ...and the name not read, so" hasline "$tmp/r.out" "_other_weak -> not read"
# The opcodes that set a library ordinal (0x11), a special ordinal (0x3e, a flat lookup) or the bind type (0x51, a
# pointer) hold their value in the opcode byte: each, read before a bind (0x90), leaves that bind in its slot.
python3 -I "$here/make_macho.py" "$tmp/ordinals.dylib" '{"weak_bind_hex": "405f61001190405f62003e90405f6300519000"}'
check "slots: the library, special-ordinal and bind-type opcodes take no operand" \
  run 0 "$tmp/ord.out" python3 -I "$here/macos_bindings.py" slots "$tmp/ordinals.dylib" arm64
check "  ...so the bind after each keeps its slot" test "$(cat "$tmp/ord.out")" = "$(printf '0 _a\n8 _b\n10 _c')"
python3 -I "$here/make_macho.py" "$tmp/threaded.dylib" '{"weak_bind_hex": "d0"}'
check "an opcode the tool does not read fails, naming it" \
  run 1 "$tmp/t.out" python3 -I "$here/macos_bindings.py" slots "$tmp/threaded.dylib" arm64
check "  ...with the opcode" has "$tmp/t.out" "weak-bind opcode 0xd0 is not one this tool reads"
# The bounds the slots rest on. A third segment command (0x19) of only its 8-byte header holds no vmaddr.
python3 -I "$here/make_macho.py" "$tmp/short-segment.dylib" '{"command": 25}'
check "a segment command shorter than its fields fails, saying so" \
  run 1 "$tmp/ss.out" python3 -I "$here/macos_bindings.py" slots "$tmp/short-segment.dylib" arm64
check "  ...with the reason" has "$tmp/ss.out" "an LC_SEGMENT_64 is 8 bytes, shorter than its fields"
# A bind after a segment and offset (0x72, 0) that names segment 2, one past the slice's two.
python3 -I "$here/make_macho.py" "$tmp/no-segment.dylib" '{"weak_bind_hex": "405f706c616365686f6c64657200720090"}'
check "a weak bind into a segment the slice lacks fails, saying so" \
  run 1 "$tmp/ns.out" python3 -I "$here/macos_bindings.py" slots "$tmp/no-segment.dylib" arm64
check "  ...naming the segment" has "$tmp/ns.out" "a weak bind names segment 2 of 2"
# A symbol name (0x40) the stream ends before its NUL: the zeros that pad __LINKEDIT after the stream do not end it.
python3 -I "$here/make_macho.py" "$tmp/unterminated.dylib" '{"weak_bind_hex": "405f706c616365686f6c646572"}'
# Bounded, so that a walk the guard no longer stops fails rather than hangs.
check "a symbol name that runs past the weak-bind stream fails, saying so" \
  run 1 "$tmp/un.out" bounded python3 -I "$here/macos_bindings.py" slots "$tmp/unterminated.dylib" arm64
check "  ...with the reason" has "$tmp/un.out" "a symbol name runs past the weak-bind stream"
# A weak-bind stream whose size in LC_DYLD_INFO_ONLY runs past the end of the slice.
python3 -I "$here/make_macho.py" "$tmp/past-slice.dylib" '{"weak_bind_hex": "90", "weak_bind_size": 1048576}'
check "a weak-bind stream that lies outside the slice fails, saying so" \
  run 1 "$tmp/ps.out" python3 -I "$here/macos_bindings.py" slots "$tmp/past-slice.dylib" arm64
check "  ...with the reason" has "$tmp/ps.out" "the weak-bind opcodes lie outside the slice"

# fs_usage -w -f pathname lines: time, call, its fields, the path, the elapsed time (W when it waited), the thread.
# HOME and the current directory are beside the host's own directory, as in the weak-bind test.
line() { printf '10:00:00.%06d  %-12s %-10s %s  0.000004 %s weak_host.4242\n' "$1" "$2" "$3" "$4" "${5:-}"; }
{
  line 1 access '(___F)' "$tmp/home/not-a-configuration-file"
  line 2 open 'F=3' /usr/lib/libc++.1.dylib
  line 3 open '[  2]' "$tmp/host/d/amazon.redshiftodbc.ini" W
  line 4 open 'F=4' /dev/null
  # The CA file the smoke names, which is the connection's own, and the built-in root.crt the driver looks for
  # beside itself when a CA file is named.
  line 4 open 'F=4' "$tmp/ca.pem"
  line 4 stat64 '[  2]' "$tmp/host/d/root.crt"
  line 5 open 'F=5' "$tmp/host/weak_before.dylib"
  line 6 stat64 '[  2]' "$tmp"
  line 7 open '[  2]' /AppleInternal/XBS/.isChrooted
  line 8 open 'F=6' /Users/someone/.CFUserTextEncoding
  line 9 stat64 '[  2]' /proc/4242/exe
  # A file under each other system prefix the tool exempts, in each form a trace may show, so that dropping any
  # prefix leaves its path unlisted.
  for system in /System/Library/placeholder /usr/share/placeholder /Library/Apple/placeholder \
    /private/var/db/timezone/placeholder /var/db/timezone/placeholder /private/var/db/dyld/placeholder \
    /var/db/dyld/placeholder /usr/libexec/rosetta/placeholder /private/var/db/oah/placeholder \
    /var/db/oah/placeholder; do
    line 10 stat64 '[  2]' "$system"
  done
  # The host's own file, named through the data volume, which holds it: read as the path it backs.
  line 10 open 'F=7' "/System/Volumes/Data$tmp/host/weak_slots.dylib"
  # A framework from the system cryptex, which macOS serves the system's frameworks from, and that prefix as
  # fs_usage cuts a long path short at its end.
  line 10 open 'F=8' /System/Volumes/Preboot/Cryptexes/OS/System/Library/Frameworks/Placeholder.framework/Placeholder
  line 10 stat64 '[  2]' /System/Volumes/Preboot/Cryptex
  printf '10:00:00.000010  open  F=3  /Users/someone/unlisted  0.000004   another.99\n'
} > "$tmp/trace"
files() { python3 -I "$here/macos_config_files.py" "$1" "$policy" "$driver" "$tmp/home" "$tmp/cwd" weak_host \
  "$tmp/home/not-a-configuration-file" "$tmp/ca.pem"; }
check "a trace of the driver's own ini and root.crt, the CA file, the system's files and the host's own passes" \
  run 0 "$tmp/f.out" files "$tmp/trace"
check "  ...naming the rows it met by their location" hasline "$tmp/f.out" \
  "rows the trace met: <driver-dir>/amazon.redshiftodbc.ini, <driver-dir>/root.crt"
# Then: a path on the data volume is classified as the path it backs, and the rest of /System/Volumes/ is not the
# system's files: outside the system cryptex on the Preboot volume, beside it under a longer name, or a cut short
# path that does not reach past the Preboot volume's own directory, that directory itself included.
for unlisted in /Library/Preferences/unlisted.plist "$tmp/home/Library/unlisted-placeholder" \
  "$tmp/cwd/unlisted-placeholder" /System/Volumes/Data/Library/Preferences/unlisted.plist \
  /System/Volumes/Preboot/unlisted-placeholder /System/Volumes/Preboot/Cryptexes-placeholder \
  /System/Volumes/Prebo /System/Volumes/Preboot/; do
  { cat "$tmp/trace"; line 11 open '[  2]' "$unlisted"; } > "$tmp/unlisted"
  check "a path no row lists fails, named: ${unlisted#"$tmp"/}" run 1 "$tmp/u.out" files "$tmp/unlisted"
  check "  ...with the path" has "$tmp/u.out" "unlisted: $unlisted"
done
{
  cat "$tmp/trace"
  line 11 stat64 '[  2]' /private/etc/krb5.conf
  line 12 open '[  2]' "$tmp/home/.aws/config"
  line 13 stat64 '[  2]' /private/tmp/krb5cc_501
  line 14 stat64 '[  2]' /private/var/tmp/krb5_42.rcache2
  line 15 open '[  2]' "/System/Volumes/Data$tmp/home/.aws/credentials"
} > "$tmp/rows"
check "a trace that meets another row before a connection fails, saying so" run 1 "$tmp/o.out" files "$tmp/rows"
met="/etc/krb5.conf, /tmp/krb5cc_%{uid}, /var/tmp/krb5_%lu.rcache2, <driver-dir>/amazon.redshiftodbc.ini"
met+=", <driver-dir>/root.crt"
check "  ...each row once, by its location, /private/etc, /private/tmp and /private/var as /etc, /tmp and /var" \
  hasline "$tmp/o.out" "rows the trace met: $met, ~/.aws/config, ~/.aws/credentials"
check "  ...with the reason" has "$tmp/o.out" "opened or probed rows other than its own"
grep -v not-a-configuration-file "$tmp/trace" > "$tmp/no-control"
check "a trace without the control path fails, saying so" run 1 "$tmp/c.out" files "$tmp/no-control"
check "  ...with the reason" has "$tmp/c.out" "the trace does not show the control path"
grep -v amazon.redshiftodbc.ini "$tmp/trace" > "$tmp/no-ini"
check "a trace without the driver's own amazon.redshiftodbc.ini fails, saying so" run 1 "$tmp/i.out" files "$tmp/no-ini"
check "  ...with the reason" has "$tmp/i.out" "does not show the driver's own probe of amazon.redshiftodbc.ini"
grep -v '/d/root.crt' "$tmp/trace" > "$tmp/no-root"
check "a trace without the driver's look for its built-in root.crt fails, saying so" run 1 "$tmp/r.out" files "$tmp/no-root"
check "  ...with the reason" has "$tmp/r.out" "does not show the driver's look for its built-in <driver-dir>/root.crt"
grep -v '/ca.pem' "$tmp/trace" > "$tmp/no-ca"
check "a trace without the CA file the smoke names fails, saying so" run 1 "$tmp/ca.out" files "$tmp/no-ca"
check "  ...with the reason" has "$tmp/ca.out" "does not show the CA file the smoke names"
# The CA file is the connection's own only at its own path: another file there is still unlisted.
{ cat "$tmp/trace"; line 11 open '[  2]' "$tmp/other-ca.pem"; } > "$tmp/other-ca"
check "  ...and another file beside it is unlisted" run 1 "$tmp/oca.out" files "$tmp/other-ca"
check "  ...named" has "$tmp/oca.out" "unlisted: $tmp/other-ca.pem"
# On a Mac, macos-hosts.sh's work directory resolves to /private/var/folders/..., so HOME's and the driver
# directory's rows are met as the trace names them, not by their /var/ form.
mac="/private/var/folders/zz/placeholder/T/tmp.layout"
{
  line 1 access '(___F)' "$mac/home/not-a-configuration-file"
  line 2 open '[  2]' "$mac/host/d/amazon.redshiftodbc.ini"
  line 3 open 'F=3' "$mac/host/weak_before.dylib"
  line 4 open 'F=4' "$mac/ca.pem"
  line 4 stat64 '[  2]' "$mac/host/d/root.crt"
} > "$tmp/mac-trace"
mac_files() { python3 -I "$here/macos_config_files.py" "$1" "$policy" "$mac/host/d/librsodbc64.dylib" "$mac/home" \
  "$mac/cwd" weak_host "$mac/home/not-a-configuration-file" "$mac/ca.pem"; }
check "a trace with a Mac's /private/var/folders layout passes" run 0 "$tmp/mac.out" mac_files "$tmp/mac-trace"
check "  ...meeting the driver's own ini and root.crt there" hasline "$tmp/mac.out" \
  "rows the trace met: <driver-dir>/amazon.redshiftodbc.ini, <driver-dir>/root.crt"
{ cat "$tmp/mac-trace"; line 4 open '[  2]' "$mac/home/.aws/config"; } > "$tmp/mac-rows"
check "  ...and one that meets a row under HOME there fails" run 1 "$tmp/mac-o.out" mac_files "$tmp/mac-rows"
check "  ...naming it" hasline "$tmp/mac-o.out" \
  "rows the trace met: <driver-dir>/amazon.redshiftodbc.ini, <driver-dir>/root.crt, ~/.aws/config"
# One argument short: no CA file.
check "macos_config_files.py refuses a wrong argument count" run 2 "$tmp/fu.out" \
  python3 -I "$here/macos_config_files.py" "$tmp/trace" "$policy" "$driver" "$tmp/home" "$tmp/cwd" weak_host \
  "$tmp/home/not-a-configuration-file"
check "  ...with its usage" has "$tmp/fu.out" "macos_config_files.py <trace> <config-files.txt> <driver> <home>"

if [[ "$(uname -s)" != Darwin ]]; then
  check "macos-hosts.sh refuses to run off macOS" run 2 "$tmp/m.out" bash "$here/macos-hosts.sh" "$tmp" "$tmp/shell"
  check "  ...saying so" has "$tmp/m.out" "macos-hosts.sh: macOS only"
fi
check "macos-hosts.sh refuses a wrong argument count" run 2 "$tmp/a.out" bash "$here/macos-hosts.sh" "$tmp"
check "  ...with its usage" has "$tmp/a.out" "usage: macos-hosts.sh"
# The refusals of a build it cannot run on, before it reads any host: on any host, with a stand-in uname that
# says Darwin.
mkdir "$tmp/darwin"
printf '#!/bin/sh\necho Darwin\n' > "$tmp/darwin/uname"
chmod +x "$tmp/darwin/uname"
build="$tmp/build"
ext="$build/extension/redshift"
mkdir -p "$ext"
hosts() { PATH="$tmp/darwin:$PATH" bash "$here/macos-hosts.sh" "$build" "$tmp/shell"; }
check "macos-hosts.sh refuses a build with no loadable extension" run 2 "$tmp/x1.out" hosts
check "  ...saying so" has "$tmp/x1.out" "has no loadable extension"
: > "$ext/redshift.duckdb_extension"
# Each of the three settings on its own: the build's identity file is there, so a script that missed one would go
# on to the platform refusal and say that instead.
echo 'platform linux_amd64' > "$ext/redshift_driver_identity.txt"
cache() { printf 'REDSHIFT_CLIENT:STRING=%s\nREDSHIFT_DRIVER:STRING=%s\nREDSHIFT_TEST_ENDPOINTS:BOOL=%s\n' "$@" \
  > "$build/CMakeCache.txt"; }
cache mock embedded ON
check "macos-hosts.sh refuses a build of another client" run 2 "$tmp/x2.out" hosts
check "  ...saying so" has "$tmp/x2.out" "is not an embedded ODBC build with the test-endpoint switch"
cache odbc none ON
check "macos-hosts.sh refuses a build without the embedded driver" run 2 "$tmp/x2.out" hosts
check "  ...saying so" has "$tmp/x2.out" "is not an embedded ODBC build with the test-endpoint switch"
cache odbc embedded OFF
check "macos-hosts.sh refuses a build without the test-endpoint switch" run 2 "$tmp/x2.out" hosts
check "  ...saying so" has "$tmp/x2.out" "is not an embedded ODBC build with the test-endpoint switch"
cache odbc embedded ON
check "macos-hosts.sh refuses a build that is not a macOS one" run 2 "$tmp/x3.out" hosts
check "  ...saying so" has "$tmp/x3.out" "is a linux_amd64 build, not a macOS one"

# The host rows' first steps, on an osx_arm64 build and stand-ins for macOS's tools, with every host variable
# unset but those a case gives: uname says Darwin on arm64; lipo reads both architectures from a host file, and
# fails on one that is missing or says it cannot be read; codesign gives the flags on a host file's first line and
# the entitlements on the rest; xcrun writes an empty -o file. The host files are not programs, so a row that
# gets past its checks fails at its run, as does the rest of the script, which reaches no host and no sudo.
mkdir "$tmp/mac" "$tmp/mac-hosts" "$ext/redshift_driver"
echo 'platform osx_arm64' > "$ext/redshift_driver_identity.txt"
python3 -I "$here/make_macho.py" "$ext/redshift_driver/librsodbc64.dylib" '{"weak_bind_hex": "405f5f5a6e776d0090"}'
cat > "$tmp/mac/uname" <<'SH'
#!/bin/sh
if [ "$1" = -m ]; then echo arm64; else echo Darwin; fi
SH
cat > "$tmp/mac/lipo" <<'SH'
#!/bin/sh
[ "$1" = -archs ] && [ -f "$2" ] && [ "$(head -n 1 "$2")" != unreadable ] || { echo "lipo: cannot read $2" >&2; exit 1; }
echo arm64 x86_64
SH
cat > "$tmp/mac/codesign" <<'SH'
#!/bin/sh
for file; do :; done
if [ "$1" = -dv ]; then echo "CodeDirectory v=20500 size=1 flags=$(head -n 1 "$file") hashes=1+0" >&2
else tail -n +2 "$file"; fi
SH
cat > "$tmp/mac/xcrun" <<'SH'
#!/bin/sh
while [ $# -gt 0 ]; do [ "$1" != -o ] || : > "$2"; shift; done
SH
chmod +x "$tmp/mac/uname" "$tmp/mac/lipo" "$tmp/mac/codesign" "$tmp/mac/xcrun"
dlv='<plist><dict><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>'
# Resolved, as a Python row resolves its host (macOS's TMPDIR is under the link /var).
hosts_dir="$(cd "$tmp/mac-hosts" && pwd -P)"
echo '0x0(none)' > "$hosts_dir/not-hardened"
echo '0x10000(runtime)' > "$hosts_dir/hardened-validating"
printf '0x10000(runtime)\n%s\n' "$dlv" > "$hosts_dir/hardened"
echo unreadable > "$hosts_dir/unreadable"
mac_hosts() { env -u REDSHIFT_TEST_DUCKDB_CLI -u REDSHIFT_TEST_PYTHON_ORG -u REDSHIFT_TEST_PYTHON_ORG_FRAMEWORKS \
  -u REDSHIFT_TEST_HOMEBREW_PYTHON -u REDSHIFT_TEST_DUCKDB_PYTHON -u REDSHIFT_TEST_REQUIRE_MACOS_HOSTS \
  -u REDSHIFT_TEST_FS_USAGE PATH="$tmp/mac:$PATH" "$@" bash "$here/macos-hosts.sh" "$build" "$tmp/shell"; }
# tapline <out file> <ok|not ok> <text>: a TAP line with that result and that text.
tapline() { sed -n "s/^$2 [0-9]* - //p" "$1" | grep -qxF -- "$3" || { echo "# no '$2 - $3' in: $(cat "$1")"; return 1; }; }
check "a host row with no host given skips" run 1 "$tmp/h1.out" mac_hosts
check "  ...saying so" tapline "$tmp/h1.out" ok "duckdb-cli # SKIP no host given"
check "  ...and fails with REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1" \
  run 1 "$tmp/h2.out" mac_hosts REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1
check "  ...saying so" tapline "$tmp/h2.out" "not ok" \
  "duckdb-cli: no host given (REDSHIFT_TEST_REQUIRE_MACOS_HOSTS=1)"
check "a host lipo cannot read fails its row, without REDSHIFT_TEST_REQUIRE_MACOS_HOSTS" \
  run 1 "$tmp/h3.out" mac_hosts REDSHIFT_TEST_DUCKDB_CLI="$hosts_dir/unreadable"
check "  ...naming it" tapline "$tmp/h3.out" "not ok" \
  "duckdb-cli: cannot read $hosts_dir/unreadable: lipo: cannot read $hosts_dir/unreadable"
check "each row's host is held to its signing state" run 1 "$tmp/h4.out" mac_hosts \
  REDSHIFT_TEST_DUCKDB_CLI="$hosts_dir/not-hardened" REDSHIFT_TEST_PYTHON_ORG="$hosts_dir/hardened-validating" \
  REDSHIFT_TEST_HOMEBREW_PYTHON="$hosts_dir/hardened"
check "  ...no runtime flag: not hardened" tapline "$tmp/h4.out" "not ok" \
  "duckdb-cli: $hosts_dir/not-hardened is not-hardened, not hardened"
check "  ...the runtime without disable-library-validation: hardened, validating" tapline "$tmp/h4.out" "not ok" \
  "python-org: $hosts_dir/hardened-validating is hardened-validating, not hardened"
check "  ...the runtime with it: hardened" tapline "$tmp/h4.out" "not ok" \
  "homebrew-python: $hosts_dir/hardened is hardened, not not-hardened"
# A host in its row's state gets past the check, to its run, which fails as the stand-in is not a program.
check "a host signed as its row wants gets past the signing check" run 1 "$tmp/h5.out" mac_hosts \
  REDSHIFT_TEST_DUCKDB_CLI="$hosts_dir/hardened"
check "  ...to its run" grep -qE '^not ok [0-9]+ - duckdb-cli: the driver did not load and fail the connect itself' \
  "$tmp/h5.out"

echo "# $n cases, $failed failed"
((failed == 0))
