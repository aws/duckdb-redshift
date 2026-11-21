#!/usr/bin/env bash
# Tool tests for the helpers the tools share, third_party/redshift_odbc/tools/lib/common.sh: the
# streamed SHA-256, which must not read a whole file into memory and must not import a module from
# the working directory, the policy-file reader, signer.txt's checksum row, the chain-shape reader,
# and the choice of the retained copies for one or both macOS platforms; verify-release_test.sh and
# the verify suites run the rest through the tools. Also the test helpers' own cases: the macOS
# suites without coreutils timeout, and bounded.sh on SIGTERM, SIGHUP and SIGINT. Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lib="$here/../../../third_party/redshift_odbc/tools/lib"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }

# A script that sources the helper and runs one of its functions, so each case runs it as the tools
# do: with fail defined, and nothing else of theirs.
cat > "$tmp/use.sh" <<EOF
set -euo pipefail
fail() { echo "\$*" >&2; exit 1; }
. "$lib/common.sh"
"\$@"
EOF

printf 'the bytes to hash\n' > "$tmp/file"
want="$(python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$tmp/file")"
got="$(bash "$tmp/use.sh" sha256 "$tmp/file")"
[[ "$got" == "$want" ]] && pass "sha256 prints a file's SHA-256" || fail "sha256 printed $got, not $want"
out="$(bash "$tmp/use.sh" sha256 "$tmp/no-such-file" 2>&1)" && status=0 || status=$?
# The reason is one line on stdout, so a caller that captures the output names its rule and this
# reason together, rather than printing a traceback and an empty detail.
if [[ "$status" == 1 && "$out" == "cannot read $tmp/no-such-file: No such file or directory" ]]; then
  pass "sha256 fails on a file it cannot read, with one line and no traceback"
else fail "sha256 on a missing file (exit $status; $out)"; fi

# The helper runs python3 with -I, so a module beside the working directory is never imported: a
# planted hashlib.py that would print another digest changes nothing.
mkdir -p "$tmp/planted"
cat > "$tmp/planted/hashlib.py" <<'EOF'
class _Digest:
    def update(self, data): pass
    def hexdigest(self): return "0" * 64
def sha256(*args, **kwargs): return _Digest()
EOF
got="$(cd "$tmp/planted" && bash "$tmp/use.sh" sha256 "$tmp/file")"
[[ "$got" == "$want" ]] && pass "a hashlib.py in the working directory is not imported (python3 -I)" ||
  fail "sha256 with a planted hashlib.py printed $got, not $want"

# It reads the file in pieces, so a file larger than the address space it is given still hashes: a
# 512 MiB sparse file under a 256 MiB limit, which reading it whole could not hold.
python3 -I -c 'import sys
with open(sys.argv[1], "wb") as f:
    f.truncate(512 << 20)' "$tmp/big"
big_want="$(python3 -I -c 'import hashlib,sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
print(h.hexdigest())' "$tmp/big")"
if out="$(bash -c 'ulimit -v 262144 2>/dev/null || exit 70; exec bash "$1" sha256 "$2"' _ "$tmp/use.sh" "$tmp/big" 2>&1)"; then
  [[ "$out" == "$big_want" ]] && pass "sha256 hashes a 512 MiB file under a 256 MiB address-space limit" ||
    fail "sha256 of the large file printed $out, not $big_want"
elif [[ "$?" == 70 ]]; then
  n=$((n+1)); echo "ok $n - sha256 hashes a large file under an address-space limit # SKIP this shell cannot set ulimit -v"
else fail "sha256 under ulimit -v (output: $out)"; fi

# policy_lines: comments, blank lines and repeated spaces go; a missing file names itself.
printf '# a comment\n\n  one   two  # trailing\nthree\n' > "$tmp/policy.txt"
got="$(bash "$tmp/use.sh" policy_lines "$tmp" policy.txt)"
[[ "$got" == "one two"$'\n'"three" ]] && pass "policy_lines drops comments and blank lines and squeezes spaces" ||
  fail "policy_lines printed: $got"
out="$(bash "$tmp/use.sh" policy_lines "$tmp" no-such.txt 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == "policy: no-such.txt is missing" ]] &&
  pass "policy_lines fails naming a policy file that is missing" || fail "policy_lines missing (exit $status; $out)"
# A file that is there but cannot be read fails with its own rule, not with awk's message.
printf 'one\n' > "$tmp/unreadable.txt"; chmod 000 "$tmp/unreadable.txt"
out="$(bash "$tmp/use.sh" policy_lines "$tmp" unreadable.txt 2>&1)" && status=0 || status=$?
chmod 644 "$tmp/unreadable.txt"
if [[ "$status" == 1 && "$out" == "policy: unreadable.txt is unreadable" ]]; then
  pass "policy_lines fails naming a policy file it cannot read"
elif [[ "$(id -u)" == 0 ]]; then
  n=$((n+1)); echo "ok $n - policy_lines fails naming a policy file it cannot read # SKIP root reads a 000 file"
else fail "policy_lines unreadable (exit $status; $out)"; fi

# signer_checksum: the one checksum row's algorithm, which the fetch, the macOS verification and
# the pkgutil suite read; a missing signer.txt fails with policy_lines' message alone, and any
# other checksum row with one message.
signer_msg='policy: policy/macos/signer.txt must have exactly one checksum row, "checksum <algorithm>"'
mkdir -p "$tmp/signer/policy/macos"
printf '# rows\nteam 94KV3E626L\nchecksum   sha1  # the payload TOC\ninstaller Developer ID Installer: X\n' \
  > "$tmp/signer/policy/macos/signer.txt"
out="$(bash "$tmp/use.sh" signer_checksum "$tmp/signer" 2>&1)" && status=0 || status=$?
[[ "$status" == 0 && "$out" == sha1 ]] && pass "signer_checksum prints signer.txt's checksum algorithm" ||
  fail "signer_checksum (exit $status; $out)"
for rows in "checksum sha1 sha256" "team 94KV3E626L" "checksum" "checksum sha1|checksum sha256" "checksum SHA1" "checksum sha-1"; do
  printf '%s\n' "$rows" | tr '|' '\n' > "$tmp/signer/policy/macos/signer.txt" # | between rows
  out="$(bash "$tmp/use.sh" signer_checksum "$tmp/signer" 2>&1)" && status=0 || status=$?
  [[ "$status" == 1 && "$out" == "$signer_msg" ]] && pass "signer_checksum refuses the rows '$rows' with one message" ||
    fail "signer_checksum with the rows '$rows' (exit $status; $out)"
done
rm -- "$tmp/signer/policy/macos/signer.txt"
out="$(bash "$tmp/use.sh" signer_checksum "$tmp/signer" 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == "policy: policy/macos/signer.txt is missing" ]] &&
  pass "signer_checksum fails on a missing signer.txt with policy_lines' message alone" ||
  fail "signer_checksum without signer.txt (exit $status; $out)"

# no_trust_variables unsets every variable that could add a trusted certificate or other OpenSSL
# configuration, and nothing else.
cat > "$tmp/trust.sh" <<EOF
set -euo pipefail
fail() { echo "\$*" >&2; exit 1; }
. "$lib/common.sh"
no_trust_variables
env
EOF
trust_env="$(env SSL_CERT_FILE=a SSL_CERT_DIR=b SSL_CERT_URI=c OPENSSL_CONF=d OPENSSL_CONF_INCLUDE=e \
  OPENSSL_MODULES=f OPENSSL_ENGINES=g KEEP_ME=h bash "$tmp/trust.sh")"
left="$(grep -cE '^(SSL_CERT_|OPENSSL_)' <<<"$trust_env" || true)"
kept="$(grep -c '^KEEP_ME=h' <<<"$trust_env" || true)"
[[ "$left" == 0 && "$kept" == 1 ]] &&
  pass "no_trust_variables unsets the OpenSSL variables and leaves the rest" ||
  fail "no_trust_variables left $left OpenSSL variables, kept $kept of KEEP_ME"

# chain_shape: one word per depth= line, u for "(untrusted)", t otherwise; other lines ignored.
got="$(bash "$tmp/use.sh" chain_shape "$(printf '%s\n' 'depth=0: CN = leaf (untrusted)' 'depth=1: CN = ca (untrusted)' \
  'depth=2: CN = root' 'OK' 'Chain:')")"
[[ "$got" == "depth=0u depth=1u depth=2t " ]] && pass "chain_shape reads openssl's -show_chain lines" ||
  fail "chain_shape printed '$got'"

# retained: the row of any platform it is given names the retained package, so one macOS run finds
# its package from either macOS row; a lock without one says so and the run downloads.
lock="$here/../../../third_party/redshift_odbc/driver.lock"
mac_sha="$(awk '$2 == "driver" && $1 == "osx_arm64" { print $6 }' "$lock")"
mac_url="$(awk '$2 == "driver" && $1 == "osx_arm64" { print $5 }' "$lock")"
lock_version="$(awk '$1 == "version" { print $2 }' "$lock")"
mkdir -p "$tmp/mirror"
cat > "$tmp/retained.sh" <<EOF
set -euo pipefail
fail() { echo "\$*" >&2; exit 1; }
platform=osx_arm64
. "$lib/common.sh"
retained "\$@"
echo "mirror=\$mirror sha=\$want_package_sha"
EOF
got="$(RS_DRIVER_MIRROR="$tmp/mirror" RS_DRIVER_LOCK="$lock" bash "$tmp/retained.sh" "$lock_version" "$mac_url" osx_amd64 osx_arm64 2>&1)"
[[ "$got" == "mirror=$tmp/mirror sha=$mac_sha" ]] && pass "retained takes the package hash from a macOS row for both macOS platforms" ||
  fail "retained for macOS printed: $got"
grep -v '^osx_amd64 ' "$lock" > "$tmp/arm-only.lock"
got="$(RS_DRIVER_MIRROR="$tmp/mirror" RS_DRIVER_LOCK="$tmp/arm-only.lock" bash "$tmp/retained.sh" "$lock_version" "$mac_url" osx_amd64 osx_arm64 2>&1)"
[[ "$got" == "mirror=$tmp/mirror sha=$mac_sha" ]] && pass "retained finds the package from the one macOS row a lock keeps" ||
  fail "retained with one macOS row printed: $got"
grep -v '^osx_' "$lock" > "$tmp/linux-only.lock"
got="$(RS_DRIVER_MIRROR="$tmp/mirror" RS_DRIVER_LOCK="$tmp/linux-only.lock" bash "$tmp/retained.sh" "$lock_version" "$mac_url" osx_amd64 osx_arm64 2>&1)"
[[ "$got" == *"has no osx_amd64 or osx_arm64 driver row; downloading from ${mac_url%/*}"*"mirror= sha=" ]] &&
  pass "a lock without a macOS row makes retained say so and choose the download" || fail "retained without a macOS row printed: $got"
got="$(RS_DRIVER_MIRROR="$tmp/mirror" RS_DRIVER_LOCK="$lock" bash "$tmp/retained.sh" "$lock_version" "${mac_url%.pkg}-other.pkg" osx_arm64 2>&1)" &&
  status=0 || status=$?
[[ "$status" == 1 && "$got" == "mirror: the recorded lock's osx_arm64 row is for $mac_url, not ${mac_url%.pkg}-other.pkg" ]] &&
  pass "retained refuses a recorded row for another package URL" || fail "retained with another URL (exit $status; $got)"
got="$(RS_DRIVER_MIRROR="$tmp/mirror" RS_DRIVER_LOCK="$lock" bash "$tmp/retained.sh" "$lock_version" "${mac_url%.pkg}-other.pkg" osx_amd64 osx_arm64 2>&1)" &&
  status=0 || status=$?
[[ "$status" == 1 && "$got" == "mirror: the recorded lock's osx_amd64 row is for $mac_url, not ${mac_url%.pkg}-other.pkg" ]] &&
  pass "retained names the row it read, not the caller's platform" || fail "retained's row in the URL message (exit $status; $got)"
got="$(env -u RS_DRIVER_MIRROR RS_DRIVER_LOCK="$lock" bash "$tmp/retained.sh" "$lock_version" "$mac_url" osx_arm64 2>&1)"
[[ "$got" == "mirror= sha=" ]] && pass "without RS_DRIVER_MIRROR retained chooses the download, silently" ||
  fail "retained without a mirror printed: $got"

# A host without coreutils timeout runs the macOS suites: lib/download.sh needs only curl, so
# neither macos-test-lib.sh nor verify-test-lib.sh has a timeout gate left. This PATH holds the two
# tools verify-test-lib.sh does require, python3 and openssl, but no timeout, so the suite goes
# past every gate and stops later, at chmod, which this PATH has not got and a macOS host has.
# Either gate would answer first: macos-test-lib.sh's with a SKIP and exit 0, verify-test-lib.sh's
# naming timeout as required by the suite. Any other early failure fails the case too.
mkdir -p "$tmp/no-timeout"
for c in bash env python3 openssl dirname basename mktemp rm mkdir cp mv cat sed awk grep tr od head tail sort wc date uname ls cut id; do
  [[ ! -x "$(command -v "$c" 2>/dev/null)" ]] || ln -sf "$(command -v "$c")" "$tmp/no-timeout/"
done
out="$(PATH="$tmp/no-timeout" bash "$here/verify-macos-library_test.sh" 2>&1)" && status=0 || status=$?
[[ "$status" == 127 && "$out" == *"verify-test-lib.sh: line "*": chmod: command not found" && "$out" != *timeout* &&
  "$out" != *SKIP* ]] &&
  pass "a macOS suite without coreutils timeout passes every tool gate and stops at chmod, which its PATH lacks" ||
  fail "a macOS suite without timeout (exit $status; $out)"

# macos-pkgutil_test.sh on a macOS host without timeout: it downloads the locked package through
# lib/download.sh when neither the cache nor RS_DRIVER_MIRROR holds it, and it fails rather than
# skipping when the download cannot be made; a mirror copy without the lock's SHA-256 fails it
# without any download. Run in a copy of the tree with an empty cache, a stand-in uname that says
# Darwin, a pkgutil that must not run and a stand-in curl that records each call and refuses, so
# nothing is fetched here.
mkdir -p "$tmp/stub-curl"
printf '#!/bin/sh\necho called >> "%s"\nexit 99\n' "$tmp/curl-called" > "$tmp/stub-curl/curl"; chmod +x "$tmp/stub-curl/curl"
pk="$tmp/pkgutil-repo"
mkdir -p "$pk/test/client/driver" "$pk/third_party/redshift_odbc/policy/macos" "$tmp/darwin" "$tmp/tampered" "$tmp/empty-mirror"
cp "$here/macos-pkgutil_test.sh" "$here/release-cache.sh" "$pk/test/client/driver/"
cp -R "$lib/.." "$pk/third_party/redshift_odbc/tools"
cp "$lib/../../driver.lock" "$pk/third_party/redshift_odbc/"
printf '#!/bin/sh\necho Darwin\n' > "$tmp/darwin/uname"
printf '#!/bin/sh\necho "stand-in pkgutil: must not run" >&2\nexit 99\n' > "$tmp/darwin/pkgutil"
chmod +x "$tmp/darwin/uname" "$tmp/darwin/pkgutil"
ln -sf "$tmp/stub-curl/curl" "$tmp/darwin/curl"
pk_sha="$(awk '$1 == "osx_arm64" { print $6 }' "$pk/third_party/redshift_odbc/driver.lock")"
rm -f -- "$tmp/curl-called"
out="$(env -u RS_DRIVER_LOCK RS_DRIVER_MIRROR="$tmp/empty-mirror" PATH="$tmp/darwin:$tmp/no-timeout" \
  bash "$pk/test/client/driver/macos-pkgutil_test.sh" 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == *"not ok 1 - "*"could not be copied or downloaded"* && "$out" != *SKIP* && -e "$tmp/curl-called" &&
  -z "$(ls -A "$pk/build/driver-package-cache/packages")" ]] &&
  pass "the pkgutil suite without timeout tries the download, fails when it cannot, and caches nothing" ||
  fail "the pkgutil suite with no package (exit $status; $out)"
printf 'tampered\n' > "$tmp/tampered/$pk_sha"
rm -f -- "$tmp/curl-called"
out="$(env -u RS_DRIVER_LOCK RS_DRIVER_MIRROR="$tmp/tampered" PATH="$tmp/darwin:$tmp/no-timeout" bash "$pk/test/client/driver/macos-pkgutil_test.sh" 2>&1)" &&
  status=0 || status=$?
[[ "$status" == 1 && "$out" == *"not ok 1 - "*"could not be copied or downloaded with the lock's SHA-256"* && "$out" != *"stand-in pkgutil"* &&
  ! -e "$tmp/curl-called" && -z "$(ls -A "$pk/build/driver-package-cache/packages")" ]] &&
  pass "the pkgutil suite fails on a mirror copy without the lock's SHA-256, downloading nothing and caching nothing" ||
  fail "the pkgutil suite with a tampered mirror copy (exit $status; $out)"

# get reads at most one byte past the bound from a retained copy (lib/common.sh's head -c), so a
# retained file far over it is refused without being copied whole.
mkdir -p "$tmp/big-mirror"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(64 << 20)' "$tmp/big-mirror/file"
cat > "$tmp/get.sh" <<EOF
set -euo pipefail
fail() { echo "\$*" >&2; exit 1; }
. "$lib/common.sh"
mirror="$tmp/big-mirror"
get https://example.invalid/x file "$tmp/got" 1024
EOF
out="$(bash "$tmp/get.sh" 2>&1)" && status=0 || status=$?
got_size="$(python3 -I -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$tmp/got" 2>/dev/null || echo none)"
[[ "$status" == 1 && "$out" == "mirror: $tmp/big-mirror/file is larger than 1024 bytes" && "$got_size" == 1025 ]] &&
  pass "get refuses a retained file over its bound after reading one byte past it" ||
  fail "get over the bound (exit $status; $out; wrote $got_size bytes)"
rm -f -- "$tmp/big-mirror/file" "$tmp/got"
# A retained copy must be a regular file, not a link to one: a link could point anywhere, and the
# signature, certificate and notices are not hash-checked before use.
printf 'small\n' > "$tmp/target"; ln -s "$tmp/target" "$tmp/big-mirror/file"
out="$(bash "$tmp/get.sh" 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == "mirror: $tmp/big-mirror/file is missing or is not a regular file" && ! -e "$tmp/got" ]] &&
  pass "get refuses a symbolic link at a retained path, even to a small regular file" ||
  fail "get of a symbolic link (exit $status; $out)"
rm -f -- "$tmp/big-mirror/file" "$tmp/target" "$tmp/got"

# policy_lines reads in the C locale (macOS's awk stops on a byte its UTF-8 locale cannot decode): an
# awk stand-in that refuses to run in any other locale still reads the file.
mkdir -p "$tmp/c-awk"
printf '#!/bin/sh\n[ "$LC_ALL" = C ] || { echo "stand-in awk: not the C locale" >&2; exit 2; }\nexec %s "$@"\n' "$(command -v awk)" > "$tmp/c-awk/awk"
chmod +x "$tmp/c-awk/awk"
# stderr is kept apart: bash warns on it where the host has no en_US.UTF-8 locale.
got="$(LC_ALL=en_US.UTF-8 PATH="$tmp/c-awk:$PATH" bash "$tmp/use.sh" policy_lines "$tmp" policy.txt 2> "$tmp/locale.err")" &&
  status=0 || status=$?
[[ "$status" == 0 && "$got" == "one two"$'\n'"three" ]] && pass "policy_lines runs awk in the C locale" ||
  fail "policy_lines in a UTF-8 locale (exit $status; $got; $(cat "$tmp/locale.err"))"

# bounded.sh: a SIGTERM, SIGHUP or SIGINT to the wrapper stops the command's whole process group, as
# the time limit does, so no child it forked outlives the wrapper. The command records the wrapper's
# PID, its own (the group's, as it leads a new session) and its child's. In the TERM-HUP case a SIGHUP
# follows the SIGTERM at once; Python runs whichever handler it takes first, so either status will do.
# The traced cases put a python3 first on PATH that runs bounded's script under a tracer, which sends
# the signals TRACE_SIGNALS names at the point TRACE_AT names and leaves a mark (the child's PID), so a
# case that never gets there fails: `kill`, the line of the kill in the finally block, after a first
# SIGTERM or after the time limit; `start`, inside subprocess's start of the child, after its fork and
# before the wrapper holds it.
# shellcheck source=bounded.sh
source "$here/bounded.sh"
# running <pid>: alive and not a zombie no one has reaped yet (a container's PID 1 may never reap
# it); without ps, alive is enough.
running() {
  local st
  kill -0 "$1" 2>/dev/null || return 1
  command -v ps >/dev/null || return 0
  if st="$(ps -o stat= -p "$1" 2>/dev/null)"; then
    [[ "${st// /}" != Z* ]]
  else
    # ps reported nothing: gone if it was reaped between the two looks; if it is still there, this ps
    # cannot say (a ps without -o stat=), so count it as running rather than pass a live child.
    kill -0 "$1" 2>/dev/null
  fi
}
mkdir -p "$tmp/traced"
cat > "$tmp/traced/python3" <<EOF
#!$(command -v python3) -I
import os, signal, sys
code = sys.argv[3]
sys.argv = ["-c"] + sys.argv[4:]
at, signals = os.environ["TRACE_AT"], os.environ["TRACE_SIGNALS"].split()
kill_line = next(n for n, line in enumerate(code.split("\\n"), 1) if "p.poll() is None" in line)
sent = []
def send(pid):
    sent.append(True)
    open(os.environ["TRACE_MARK"], "w").write("%d\\n" % pid)
    for name in signals:
        os.kill(os.getpid(), getattr(signal, "SIG" + name))
def trace(frame, event, arg):
    if sent or event != "line":
        return trace
    if at == "kill" and frame.f_code.co_filename == "<bounded>" and frame.f_lineno == kill_line:
        send(frame.f_globals["p"].pid)
    # Inside Popen, once the child is forked: subprocess sets _child_created then, before Popen returns.
    elif (at == "start" and frame.f_code.co_name == "_execute_child" and
          getattr(frame.f_locals.get("self"), "_child_created", False)):
        send(frame.f_locals["self"].pid)
    return trace
sys.settrace(trace)
exec(compile(code, "<bounded>", "exec"), {"__name__": "__main__"})
EOF
chmod +x "$tmp/traced/python3"
# <case> <signal sent from here, or -> <traced point, or -> <traced signals> <BOUNDED_SECONDS> <exit status>
while read -r case sig at signals seconds want; do
  pids="$tmp/bounded-$case"; mark="$tmp/bounded-$case.mark"; path="$PATH"
  [[ "$at" == - ]] || path="$tmp/traced:$PATH"
  case "$case" in
  TERM-HUP) what="SIGTERM and at once SIGHUP" ;;
  TERM-again) what="SIGTERM, then SIGTERM and SIGHUP in its finally block" ;;
  timeout-then-TERM) what="its time limit, then SIGTERM in its finally block" ;;
  *-while-starting) what="SIG${case%%-*} while the command is being started" ;;
  *) what="SIG$case" ;;
  esac
  # "|| exit": under set -e, bash 4.2 gives a background subshell whose last command fails status 1.
  ( PATH="$path" TRACE_AT="$at" TRACE_SIGNALS="${signals//,/ }" TRACE_MARK="$mark" BOUNDED_SECONDS="$seconds" \
      bounded bash -c 'sleep 60 & echo "$PPID $$ $!" > "$1.part"; mv "$1.part" "$1"; wait' _ "$pids" || exit ) &
  sub=$!
  # The start case's command is killed before it can write its PIDs; the tracer's mark names its group.
  for _ in $(seq 100); do [[ -s "$pids" || -s "$mark" ]] && break; sleep 0.1; done
  wrapper=; group=; child=
  read -r wrapper group child 2>/dev/null < "$pids" || true
  [[ "$at" != start ]] || { read -r group 2>/dev/null < "$mark" || true; }
  if [[ -z "$group" ]]; then
    kill -s KILL "$sub" 2>/dev/null || true; fail "bounded's command never started ($what)"; continue
  fi
  start=$SECONDS
  for s in ${sig//,/ }; do [[ "$s" == - ]] || kill -s "$s" "$wrapper" 2>/dev/null || true; done
  wait "$sub" && status=0 || status=$?
  if ((SECONDS - start > 10)); then fail "bounded took $((SECONDS - start)) s to stop after $what"; continue; fi
  for _ in $(seq 50); do kill -0 -- "-$group" 2>/dev/null && { [[ -z "$child" ]] || running "$child"; } || break; sleep 0.1; done
  if kill -0 -- "-$group" 2>/dev/null && { [[ -z "$child" ]] || running "$child"; }; then
    kill -s KILL -- "-$group" 2>/dev/null || true
    fail "bounded leaves its command running after $what (exit $status)"
  elif [[ "$at" != - && ! -s "$mark" ]]; then
    fail "the tracer never reached its point, so it sent nothing ($what, exit $status)"
  elif [[ " ${want//,/ } " != *" $status "* ]]; then
    fail "bounded on $what exited $status, not ${want//,/ or }"
  else
    pass "bounded stops its command's process group on $what and exits $status"
  fi
done <<'CASES'
TERM TERM - - 60 143
HUP HUP - - 60 129
INT INT - - 60 130
TERM-HUP TERM,HUP - - 60 129,143
TERM-again TERM kill TERM,HUP 60 143
timeout-then-TERM - kill TERM 1 124
TERM-while-starting - start TERM 60 143
INT-while-starting - start INT 60 130
CASES
# A command that cannot be started is Popen's own error: no process to kill, and no other error after it.
out="$(bounded "$tmp/no-such-tool" 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == *FileNotFoundError* && "$out" != *AttributeError* ]] &&
  pass "bounded on a command that does not exist exits 1 with Popen's FileNotFoundError" ||
  fail "bounded on a missing command (exit $status; $out)"

# notice_rows refuses a package that holds a member named like a notice, naming its path.
out="$(bash "$tmp/use.sh" notice_rows 2.3.0.0 "$tmp" "$tmp" "LICENSE ./a/LICENSE" 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == "notices: the package holds ./a/LICENSE, named like a notice but not the one regular copy beside the driver; the release tag's copy ships" ]] &&
  pass "notice_rows refuses a notice inside the package" || fail "notice_rows with a found notice (exit $status; $out)"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
