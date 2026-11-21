#!/usr/bin/env bash
# The host probe. With a stand-in for the probe: host-probe.sh prints each host fact (the kernel and
# architecture as uname gives them; the OS and C library, or `unknown` without an os-release file or a
# getconf that answers; libkeyutils present, absent or unknown, from a stand-in ldconfig; REDSHIFT_DRIVER_DIR,
# or `unset`; /tmp's mount options and the user namespace's first uid_map line, each `unknown` without /proc or
# when awk cannot read it) and the probe's lines as tab-separated `<check> <ok|fail|skip> <detail>`, adds
# the file system type of the load directory and of each base with a directory, a skipped one's too (`unknown`
# where stat fails, as BSD stat does), keeps the probe's exit status, removes its working directory, reports a
# killed probe, and refuses any argument as a usage error. The gate that decides whether this host can load the
# driver, on fixtures. On an embedded build, with the real probe (make test_driver builds it before the tool
# tests), with only PATH, TMPDIR and a private HOME: it reports the platform, loads the driver into
# REDSHIFT_DRIVER_DIR, reports the base, and reports nothing after the closed-port smoke, which fails, saying
# so, when there is no local port; a tab, newline or carriage return in a detail becomes a space; a refused
# base is a failed load, naming it, and exit 1; an argument, a whole --live call among them, is a usage error;
# the per-user cache is reported as the loader resolves it, or refuses it. On a host without libkeyutils.so.1
# where the loader looks for it (the library cache, then the system library directories, read with ldconfig
# and the file system, not by any interpreter's own loader) the one real-probe case is the
# MISSING_PREREQUISITE load failure, and the rest skip. Without the probe those cases report SKIP, or fail
# with RS_REQUIRE_PROBE=1, which make test_driver sets on an embedded Linux build; a run of this suite
# against a probe that is not there checks both. RS_TEST_PROBE names the probe (default: the embedded build's).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tool="$here/../../../third_party/redshift_odbc/tools/host-probe.sh"
probe="${RS_TEST_PROBE:-$here/../../../build/release/extension/redshift/redshift_driver_probe}"
n=0; failed=0
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}
tmp="$(mktemp -d)"
tmp="$(cd "$tmp" && pwd -P)"
trap 'chmod -R u+rwx "$tmp" 2>/dev/null; rm -rf -- "${tmp:?}"' EXIT
has_line() { grep -qE "^$2	$3	$4" <<<"$1"; }
three_fields() { ! grep -vE "^[a-z_]+	(ok|fail|skip)	[^	]*$" <<<"$1"; }
one_line() { grep -qxF "$2" <<<"$1" && three_fields "$1"; } # one_line <output> <the exact line it must hold>
# A directory name with a tab, a newline and a carriage return in it, which must each come out as a space.
odd="$tmp/odd"$'\t'"tab"$'\n'"newline"$'\r'"return"

# A stand-in probe that records the arguments it is given and prints the lines the real one does.
mkdir -p "$tmp/dir/ext" "$tmp/dir/cache" "$tmp/home"
cat > "$tmp/stub-probe" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$STUB_LOG/probe-args"
printf 'base\tok\tthe extension'"'"'s directory: %s/ext\n' "$STUB_DIR"
printf 'base\tskip\tthe per-user cache: %s/cache: the per-user cache is mounted noexec\n' "$STUB_DIR"
printf 'base\tskip\tthe per-user cache: : no per-user cache directory could be determined\n'
printf 'load\tok\tembedded stand-in\ndirectory\tok\t%s\n' "$STUB_DIR"
[[ -z "${STUB_SIGNAL:-}" ]] || { ulimit -c 0; kill -s "$STUB_SIGNAL" $$; }
exit "${STUB_STATUS:-0}"
EOF
chmod 755 "$tmp/stub-probe"
stub() { env STUB_LOG="$tmp" STUB_DIR="$tmp/dir" RS_PROBE="$tmp/stub-probe" "$@"; }

out="$(stub bash "$tool" 2>&1)" && status=0 || status=$?
check "exits 0 when the probe does" test "$status" = 0
check "every line has three tab-separated fields" three_fields "$out"
# The libkeyutils fact, from a stand-in ldconfig first on PATH: the library cache lists the library, lists
# others only, or (ldconfig printing nothing) cannot be read. The fact reads the cache alone, as its name says.
mkdir "$tmp/bin"
printf '#!/usr/bin/env bash\nprintf "%%s" "${STUB_LDCACHE-}"\n' > "$tmp/bin/ldconfig"
chmod 755 "$tmp/bin/ldconfig"
keyutils() { stub env PATH="$tmp/bin:$PATH" STUB_LDCACHE="$1" bash "$tool" 2>&1; }
check "reports libkeyutils present when the library cache lists libkeyutils.so.1" has_line \
  "$(keyutils $'\tlibkeyutils.so.1 (libc6,x86-64) => /lib64/libkeyutils.so.1\n')" host ok "libkeyutils_in_cache=present$"
check "reports libkeyutils absent when the library cache lists other libraries only" has_line \
  "$(keyutils $'\tlibc.so.6 (libc6,x86-64) => /lib64/libc.so.6\n')" host ok "libkeyutils_in_cache=absent$"
check "reports libkeyutils unknown when the library cache cannot be read" has_line \
  "$(keyutils '')" host ok "libkeyutils_in_cache=unknown$"
# Each host fact the probe reports: kernel and arch as uname gives them; the others present, in their form.
check "reports the kernel, as uname -sr gives it" one_line "$out" "host	ok	kernel=$(uname -sr)"
check "reports the architecture, as uname -m gives it" one_line "$out" "host	ok	arch=$(uname -m)"
check "reports the OS" has_line "$out" host ok "os=[^	]+$"
check "reports the C library" has_line "$out" host ok "glibc=(glibc [0-9]+\.[0-9]+[^	]*|unknown)$"
# The OS from the os-release file RS_OS_RELEASE names: its PRETTY_NAME, else unknown, as without the file.
printf 'NAME="Stand-in"\nPRETTY_NAME="Stand-in Linux 1"\n' > "$tmp/os-release"
printf 'NAME="Stand-in"\n' > "$tmp/os-release-unnamed"
check "reports the os-release file's PRETTY_NAME" \
  one_line "$(stub env RS_OS_RELEASE="$tmp/os-release" bash "$tool" 2>&1)" "host	ok	os=Stand-in Linux 1"
check "reports the OS unknown without a PRETTY_NAME" \
  one_line "$(stub env RS_OS_RELEASE="$tmp/os-release-unnamed" bash "$tool" 2>&1)" "host	ok	os=unknown"
check "reports the OS unknown without an os-release file" \
  one_line "$(stub env RS_OS_RELEASE="$tmp/none" bash "$tool" 2>&1)" "host	ok	os=unknown"
# The C library unknown when getconf fails, from a stand-in first on PATH.
mkdir "$tmp/no-getconf"
printf '#!/usr/bin/env bash\nexit 1\n' > "$tmp/no-getconf/getconf"
chmod 755 "$tmp/no-getconf/getconf"
check "reports the C library unknown when getconf fails" \
  one_line "$(stub env PATH="$tmp/no-getconf:$PATH" bash "$tool" 2>&1)" "host	ok	glibc=unknown"
# /tmp's mount options where the mount table can be read, so a broken read fails here; `unknown` without it (macOS).
if [[ -r /proc/self/mounts ]]; then
  check "reports /tmp's mount options" bash -c 'grep -qE "^host	ok	tmp_mount=[^	]+$" <<<"$1" &&
    ! grep -qxF "host	ok	tmp_mount=unknown" <<<"$1"' _ "$out"
else
  check "reports /tmp's mount unknown where /proc has no mount table" one_line "$out" "host	ok	tmp_mount=unknown"
fi
if [[ -r /proc/self/uid_map ]]; then
  check "reports the user namespace's first uid_map line" has_line "$out" host ok "uid_map=[0-9]+:[0-9]+:[0-9]+$"
else
  check "reports uid_map unknown where /proc has none" one_line "$out" "host	ok	uid_map=unknown"
fi
# Both /proc facts `unknown`, on any host, when the read fails: from a stand-in awk first on PATH that fails as
# awk does on a file it cannot open.
mkdir "$tmp/no-awk"
printf '#!/usr/bin/env bash\necho "awk: cannot open file" >&2\nexit 2\n' > "$tmp/no-awk/awk"
chmod 755 "$tmp/no-awk/awk"
no_awk="$(stub env PATH="$tmp/no-awk:$PATH" bash "$tool" 2>&1)"
check "reports /tmp's mount unknown when the mount table cannot be read" one_line "$no_awk" "host	ok	tmp_mount=unknown"
check "reports uid_map unknown when the user namespace's map cannot be read" one_line "$no_awk" "host	ok	uid_map=unknown"
check "reports REDSHIFT_DRIVER_DIR unset" \
  one_line "$(stub env -u REDSHIFT_DRIVER_DIR bash "$tool" 2>&1)" "host	ok	driver_dir=unset"
check "reports an empty REDSHIFT_DRIVER_DIR as empty" \
  one_line "$(stub env REDSHIFT_DRIVER_DIR= bash "$tool" 2>&1)" "host	ok	driver_dir="
check "a tab, newline or carriage return in REDSHIFT_DRIVER_DIR's fact becomes a space" \
  one_line "$(stub env REDSHIFT_DRIVER_DIR="$odd" bash "$tool" 2>&1)" "host	ok	driver_dir=$tmp/odd tab newline return"
check "passes the probe's lines through" has_line "$out" load ok "embedded stand-in"
check "adds the file system type of the directory the driver was loaded from" \
  has_line "$out" filesystem ok "$tmp/dir [a-z0-9/_-]+$"
check "adds the file system type of a base the loader may use" has_line "$out" filesystem ok "$tmp/dir/ext [a-z0-9/_-]+$"
check "adds the file system type of a skipped base's directory, without the reason" \
  has_line "$out" filesystem ok "$tmp/dir/cache [a-z0-9/_-]+$"
check "adds no file system type for a base with no directory" bash -c '! grep -q "^filesystem	ok	 " <<<"$1"' _ "$out"
# BSD stat reads -f as its format option, prints something else and fails: from a stand-in stat that does
# that, the type is `unknown` and the line keeps its three fields.
mkdir "$tmp/bsd-stat"
printf '#!/usr/bin/env bash\necho "not GNU stat: $*"\nexit 1\n' > "$tmp/bsd-stat/stat"
chmod 755 "$tmp/bsd-stat/stat"
check "a stat that prints other text and fails, as BSD stat does, gives the file system type unknown" \
  one_line "$(stub env PATH="$tmp/bsd-stat:$PATH" bash "$tool" 2>&1)" "filesystem	ok	$tmp/dir unknown"
check "runs the probe with no arguments" test "$(cat "$tmp/probe-args")" = ""
status=0; stub env STUB_STATUS=1 bash "$tool" >/dev/null 2>&1 || status=$?
check "keeps the probe's failing status" test "$status" = 1
out="$(stub env STUB_SIGNAL=SEGV bash "$tool" 2>&1)" && status=0 || status=$?
check "a probe killed by a signal fails the run, after the lines it printed" \
  bash -c '[[ "$1" == 139 ]] && grep -q "^load	ok	" <<<"$2" && grep -q "^probe	fail	the probe was killed by signal 11" <<<"$2"' _ "$status" "$out"
# The tool makes a working directory for the probe's output and removes it on exit, so a host is
# not left with one per run. TMPDIR of its own, since mktemp -d puts it there.
mkdir "$tmp/tmpdir"
stub env TMPDIR="$tmp/tmpdir" bash "$tool" >/dev/null 2>&1
check "removes its working directory afterwards" bash -c '[[ -z "$(ls -A "$1")" ]]' _ "$tmp/tmpdir"
# With the stand-in probe, so a tool that stopped refusing arguments would run it and exit 0, rather than
# stop at a missing probe with the same exit 2.
# The tool has no --live mode: it is an argument like any other.
status=0; stub bash "$tool" --live wg.example host-wg https://cp.example >/dev/null 2>&1 || status=$?
check "refuses any argument, --live among them, as a usage error" test "$status" = 2
status=0; RS_PROBE="$tmp/none" bash "$tool" >/dev/null 2>&1 || status=$?
check "fails when there is no probe" test "$status" = 2

# The real-probe cases run with only PATH, a private HOME and TMPDIR, so a variable the loader refuses
# (KRB5CCNAME, say) in the caller's environment cannot fail them.
clean() { env -i PATH="$PATH" HOME="$tmp/home" TMPDIR="${TMPDIR:-/tmp}" "$@"; }
# Whether the loader finds libkeyutils.so.1 as it finds every dependency: a file the
# library cache names for this platform (with no hardware capability; an OS ABI note is ignored, as the
# loader ignores it), else one in /lib/<triplet>, /usr/lib/<triplet>, /lib64 or /usr/lib64. The system's own
# answer, not that of whichever python3 or compiler is first on PATH. Not mirrored: the loader refuses a
# cache it cannot read, where this goes on to the directories.
# keyutils_found <arch> <ldconfig>... -- <directory>...: the same, given the architecture, the ldconfig
# programs to ask in order, and the directories, so the fixtures below can give their own.
keyutils_found() {
  local arch="$1" ldconfig cache line dir flags=libc6,x86-64; shift
  [[ "$arch" != aarch64 ]] || flags=libc6,AArch64
  while (($# > 0)) && [[ "$1" != -- ]]; do
    ldconfig="$1"; shift
    [[ -x "$ldconfig" ]] && cache="$("$ldconfig" -p 2>/dev/null)" || continue
    while IFS= read -r line; do
      [[ ! -f "${line##* => }" ]] || return 0
    done < <(grep -E "^[[:space:]]+libkeyutils\.so\.1 \($flags(, OS ABI: [^)]*)?\) => " <<<"$cache")
  done
  shift || true
  for dir in "$@"; do
    [[ ! -f "$dir/libkeyutils.so.1" ]] || return 0
  done
  return 1
}
loader_finds_keyutils() {
  local triplet
  triplet="$(uname -m)-linux-gnu"
  keyutils_found "$(uname -m)" "$(command -v ldconfig 2>/dev/null)" /sbin/ldconfig /usr/sbin/ldconfig -- \
    "/lib/$triplet" "/usr/lib/$triplet" /lib64 /usr/lib64
}
# The gate on fixtures: a stand-in ldconfig ($tmp/bin's, which prints STUB_LDCACHE) and a directory of its own.
mkdir -p "$tmp/keyutils/lib"
: > "$tmp/keyutils/libkeyutils.so.1"
entry() { printf '\tlibkeyutils.so.1 (%s) => %s\n' "$1" "$2"; }
finds() { STUB_LDCACHE="$2" keyutils_found "$1" "$tmp/bin/ldconfig" -- "${@:3}"; }
not() { ! "$@"; }
check "the gate: a cache entry for the platform whose file exists is found" \
  finds x86_64 "$(entry libc6,x86-64 "$tmp/keyutils/libkeyutils.so.1")"
check "the gate: an OS ABI note in the entry is ignored" \
  finds x86_64 "$(entry "libc6,x86-64, OS ABI: Linux 3.2.0" "$tmp/keyutils/libkeyutils.so.1")"
check "the gate: arm64 takes its own flags" finds aarch64 "$(entry libc6,AArch64 "$tmp/keyutils/libkeyutils.so.1")"
check "the gate: arm64 does not take x86-64's" not \
  finds aarch64 "$(entry libc6,x86-64 "$tmp/keyutils/libkeyutils.so.1")"
check "the gate: an entry with a hardware capability is not taken" not \
  finds x86_64 "$(entry "libc6,x86-64, hwcap: 0x0000000000000002" "$tmp/keyutils/libkeyutils.so.1")"
check "the gate: an entry whose file is gone is not taken" not \
  finds x86_64 "$(entry libc6,x86-64 "$tmp/keyutils/gone.so.1")"
check "the gate: without a cache entry, a file in a system directory is found" finds x86_64 "" "$tmp/keyutils/lib" "$tmp/keyutils"
check "the gate: without either, nothing is found" not finds x86_64 "" "$tmp/keyutils/lib"
# make test_driver sets RS_REQUIRE_PROBE=1 on an embedded Linux build, where a missing probe must fail: a
# test_driver that ran this suite before it built the probe would otherwise pass with these cases skipped.
if [[ ! -x "$probe" && "${RS_REQUIRE_PROBE:-}" == 1 ]]; then
  check "the real probe: built before this suite (RS_REQUIRE_PROBE=1)" false
elif [[ ! -x "$probe" ]]; then
  echo "ok $((n += 1)) - the real probe # SKIP no probe binary (make test_driver builds it before this suite)"
elif ! loader_finds_keyutils; then
  # Without libkeyutils.so.1 the driver cannot load, so the one case is the load failure the loader gives
  # that host.
  mkdir -m 700 "$tmp/real"
  out="$(clean REDSHIFT_DRIVER_DIR="$tmp/real" "$probe" 2>&1)" && status=0 || status=$?
  check "real probe: without libkeyutils.so.1, the load fails MISSING_PREREQUISITE naming it" \
    bash -c '[[ $1 == 1 ]] && grep -q "^load	fail	MISSING_PREREQUISITE .*libkeyutils\.so\.1" <<<"$2"' _ "$status" "$out"
  echo "ok $((n += 1)) - the real probe's other cases # SKIP this host has no libkeyutils.so.1"
else
  mkdir -m 700 "$tmp/real"
  out="$(clean REDSHIFT_DRIVER_DIR="$tmp/real" bash "$tool" 2>&1)" && status=0 || status=$?
  check "real probe: exits 0 where the driver loads" test "$status" = 0
  check "real probe: reports the platform it was built for" has_line "$out" platform ok "linux_(amd64|arm64)$"
  check "real probe: every line has three tab-separated fields" three_fields "$out"
  check "real probe: REDSHIFT_DRIVER_DIR is the only base, with its file system" \
    bash -c '[[ "$(grep -c "^base	" <<<"$1")" == 1 ]] && grep -qF "base	ok	REDSHIFT_DRIVER_DIR: $2" <<<"$1" &&
      grep -qE "^filesystem	ok	$2 [a-z0-9/_-]+$" <<<"$1"' _ "$out" "$tmp/real"
  check "real probe: loads the embedded driver" has_line "$out" load ok "embedded "
  check "real probe: names the directory it loaded from" has_line "$out" directory ok "$tmp/real/\.redshift-driver/"
  check "real probe: passes the closed-port smoke with the driver's own error" \
    has_line "$out" closed_port_smoke ok "HY000 \[Redshift\]\[ODBC Driver\]"
  # Without a local port, because socket() or bind() fails (a preloaded stand-in), the probe's smoke fails and
  # says so. With no port the probe reports before any connect, so the driver's own socket calls never run.
  for call in socket bind; do
    case "$call" in
    socket) cat <<'EOF' ;;
#include <errno.h>
int socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; errno = EMFILE; return -1; }
EOF
    bind) cat <<'EOF' ;;
#include <errno.h>
#include <sys/socket.h>
int bind(int fd, const struct sockaddr *address, socklen_t length) {
  (void)fd; (void)address; (void)length; errno = EADDRNOTAVAIL; return -1;
}
EOF
    esac > "$tmp/no-$call.c"
    if gcc -shared -fPIC -o "$tmp/no-$call.so" "$tmp/no-$call.c"; then
      noport="$(clean LD_PRELOAD="$tmp/no-$call.so" REDSHIFT_DRIVER_DIR="$tmp/real" "$probe" 2>&1)" && noport_status=0 ||
        noport_status=$?
      check "real probe: when $call() fails there is no local port: the driver loads, the smoke fails saying so, exit 1" \
        bash -c '[[ $1 == 1 ]] && grep -q "^load	ok	" <<<"$2" && grep -qxF "closed_port_smoke	fail	no local port" <<<"$2"' \
        _ "$noport_status" "$noport"
    else
      check "real probe: gcc builds the stand-in $call() library" false
    fi
  done
  # Straight from the probe, not through the tool, which appends lines of its own: the smoke is the
  # last thing the probe reports, so no live check runs after it.
  own="$(clean REDSHIFT_DRIVER_DIR="$tmp/real" "$probe" 2>&1)" || true
  check "real probe: reports nothing after the closed-port smoke" \
    bash -c '[[ "$(tail -n 1 <<<"$1" | cut -f1)" == closed_port_smoke ]]' _ "$own"
  # A base others may change is refused, and the load fails with the attempt naming it.
  mkdir -m 700 "$tmp/open"; chmod 770 "$tmp/open"
  bad="$(clean REDSHIFT_DRIVER_DIR="$tmp/open" "$probe" 2>&1)" && bad_status=0 || bad_status=$?
  check "real probe: a refused base is a failed load naming NO_USABLE_DIRECTORY and its attempt" \
    bash -c '[[ $1 == 1 ]] && grep -q "^load	fail	NO_USABLE_DIRECTORY " <<<"$2" && grep -qF "attempt	fail	$3: " <<<"$2"' \
    _ "$bad_status" "$bad" "$tmp/open"
  # The probe makes each detail one field: a tab, newline or carriage return in it (here in the base's
  # name, which the loader accepts) becomes a space, so the line stays three tab-separated fields.
  mkdir -m 700 "$odd"
  check "real probe: a tab, newline or carriage return in a detail becomes a space" \
    one_line "$(clean REDSHIFT_DRIVER_DIR="$odd" "$probe" 2>&1)" "base	ok	REDSHIFT_DRIVER_DIR: $tmp/odd tab newline return"
  # A --live call with five arguments: the probe has no such mode, so a probe that had one would
  # connect instead.
  usage_status=0; clean "$probe" --live 127.0.0.1 tester /dev/null /dev/null >/dev/null 2>&1 || usage_status=$?
  check "real probe: an argument, a whole --live call among them, is a usage error" test "$usage_status" = 2
  # The per-user cache is reported as the loader resolves it. A copy of the probe in a read-only directory
  # (its extension's directory, which it cannot write) makes the cache the base that is used, here under an
  # XDG_CACHE_HOME reached through a symbolic link: the line names the resolved directory.
  # mkdir -m 700, so the caller's umask cannot leave a directory others may write, which the loader refuses.
  mkdir -m 700 "$tmp/ro" "$tmp/xdg"; ln -s "$tmp/xdg" "$tmp/xdg-link"
  cp "$probe" "$tmp/ro/redshift_driver_probe"; chmod 555 "$tmp/ro"
  if [[ "$(id -u)" == 0 ]]; then
    # Root writes the read-only directory all the same, so the extension's directory stays the base.
    echo "ok $((n += 1)) - real probe: the per-user cache cases # SKIP as root a 0555 directory does not stop the write"
  else
    cached="$(clean XDG_CACHE_HOME="$tmp/xdg-link" "$tmp/ro/redshift_driver_probe" 2>&1)" || true
    check "real probe: the per-user cache's line names the directory the loader resolves it to" \
      one_line "$cached" "base	ok	the per-user cache: $tmp/xdg/duckdb-redshift"
    check "real probe: the per-user cache's resolved directory is where the driver loads from" \
      has_line "$cached" directory ok "$tmp/xdg/duckdb-redshift/\.redshift-driver/"
  fi
  # A probe that lives in the per-user cache: the loader tries that directory once, as the extension's.
  mkdir -m 700 "$tmp/xdg-self" "$tmp/xdg-self/duckdb-redshift"
  cp "$probe" "$tmp/xdg-self/duckdb-redshift/redshift_driver_probe"
  check "real probe: a per-user cache that is the extension's directory is a skip, saying so" \
    one_line "$(clean XDG_CACHE_HOME="$tmp/xdg-self" "$tmp/xdg-self/duckdb-redshift/redshift_driver_probe" 2>&1)" \
    "base	skip	the per-user cache: $tmp/xdg-self/duckdb-redshift: it is the extension's directory"
  # A cache the loader refuses (a parent others can write) is a skip with the loader's reason, not ok.
  chmod 777 "$tmp/xdg"
  refused="$(clean XDG_CACHE_HOME="$tmp/xdg" "$tmp/ro/redshift_driver_probe" 2>&1)" || true
  chmod 700 "$tmp/xdg"
  check "real probe: a per-user cache the loader refuses is a skip with the loader's reason" \
    bash -c '! grep -q "^base	ok	the per-user cache" <<<"$1" &&
      grep -qF "base	skip	the per-user cache: $2: the per-user cache: a parent directory" <<<"$1"' _ "$refused" "$tmp/xdg"
fi
# The missing-probe branches, from this suite run again against a probe that is not there: with
# RS_REQUIRE_PROBE=1 it fails, naming the case, and without it the case is a SKIP.
if [[ -z "${HOST_PROBE_TEST_INNER:-}" ]]; then
  self() { env "$@" HOST_PROBE_TEST_INNER=1 RS_TEST_PROBE="$tmp/no-probe" bash "${BASH_SOURCE[0]}" 2>&1; }
  inner="$(self RS_REQUIRE_PROBE=1)" && status=0 || status=$?
  check "without the probe, RS_REQUIRE_PROBE=1 fails the suite at that case" bash -c \
    '[[ $1 != 0 ]] && grep -qE "^not ok [0-9]+ - the real probe: built before this suite \(RS_REQUIRE_PROBE=1\)$" <<<"$2"' \
    _ "$status" "$inner"
  inner="$(self -u RS_REQUIRE_PROBE)" && status=0 || status=$?
  check "without the probe and RS_REQUIRE_PROBE, the case is a SKIP and the suite passes" bash -c \
    '[[ $1 == 0 ]] && grep -qE "^ok [0-9]+ - the real probe # SKIP no probe binary" <<<"$2"' _ "$status" "$inner"
fi
echo "# $n cases, $((n - failed)) passed"
((failed == 0))
