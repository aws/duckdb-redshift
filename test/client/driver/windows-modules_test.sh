#!/usr/bin/env bash
# Tool tests for test/client/driver/windows_modules.py. Its rules run through its judge command,
# which runs anywhere: a module list, a shell's output and the directories beside the extension and
# in the per-user cache are judged as the Windows check judges a shell's. Its check runs here too,
# with a stand-in shell and a stand-in for Windows' module list and cache, and so does modules. Its
# self-test, check, modules and folders commands call Windows, so here each must refuse to run, as
# must run-windows-loading.sh, which runs the self-test. Stand-ins for the Win32 calls cover
# OpenProcess, EnumProcessModules and GetModuleFileNameExW failing, a module list read again once it
# grows, one that gains a module at each read and one that keeps growing, the per-user cache's known
# folder, and the long temp folder that folders prints. Run by run-tool-tests.sh.
set -euo pipefail
unset REDSHIFT_DRIVER_DIR  # check refuses it; the refusal case sets it on its own command line

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/../../.."
tool="$here/windows_modules.py"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# shellcheck source=bounded.sh
source "$here/bounded.sh" # bounded <command>...: at most $BOUNDED_SECONDS, its process group stopped
mkdir -p "$tmp/extension" "$tmp/cache"
extension="$tmp/extension/redshift.duckdb_extension"
# shipped_refusal <file> <start>: the message of the statement that begins with <start> in <file>,
# its adjacent string literals joined and TargetPlatform() read as windows_amd64.
shipped_refusal() {
  python3 -I - "$repo/$1" "$2" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
message = []
for token in re.finditer(r'"((?:[^"\\]|\\.)*)"|(TargetPlatform\(\))|(;)', text[text.index(sys.argv[2]):]):
    if token.group(3):
        break
    message.append(token.group(1) if token.group(2) is None else "windows_amd64")
print("".join(message))
EOF
}
# The NOT_BUILT refusal and the no-trust-resolver one, read from the sources that throw them, as
# DuckDB's error text shows them.
not_built_message="$(shipped_refusal src/transport/driver/driver_not_built.cpp "result.error.message =")"
trust_message="$(shipped_refusal src/transport/odbc/trust_store_unsupported.cpp "throw IOException(")"
not_built="IO Error: the Redshift driver could not be loaded: $not_built_message"
shell_output="$not_built"
# judge_case <name> <expected-exit> <expected-output> <module path>...: judges the list and
# $shell_output against $extension and $tmp/cache; the whole output, stdout and stderr, must equal
# <expected-output>.
judge_case() {
  local name="$1" want="$2" expected="$3" out status; shift 3
  printf '%s\r\n' "$@" > "$tmp/modules.txt"
  printf '%s\n' "$shell_output" > "$tmp/output.txt"
  out="$(python3 -I "$tool" judge "$extension" "$tmp/modules.txt" "$tmp/cache" "$tmp/output.txt" 2>&1)" &&
    status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == "$expected" ]]; then pass "$name"
  else fail "$name (exit $status, wanted $want; output: $out)"; fi
}
ok="the shell attempted a connect (the NOT_BUILT refusal), loaded the extension and no driver module, and created \
or changed no driver or trust directory and no .pem file"
kernel32='C:\Windows\System32\KERNEL32.DLL'
shell='C:\duckdb\duckdb.exe'
upper="$(tr '[:lower:]/' '[:upper:]\\' <<<"$extension")"

judge_case "passes a shell that loaded the extension and system DLLs only, comparing paths as Windows does" 0 "$ok" \
  "$shell" "$kernel32" 'C:\Windows\System32\ntdll.dll' "$upper"
judge_case "reads a module path given with the \\\\?\\ prefix" 0 "$ok" "$shell" "$kernel32" "\\\\?\\$upper"
judge_case "reads a module path given with the //?/ prefix" 0 "$ok" "$shell" "$kernel32" "//?/$extension"
extension="$tmp/extension/../extension//redshift.duckdb_extension"
judge_case "reads an extension path given with .. and doubled separators" 0 "$ok" "$shell" "$kernel32" "$upper"
extension="$tmp/extension/redshift.duckdb_extension"
judge_case "reads past a blank line in the module list" 0 "$ok" "$shell" "" "$kernel32" "$extension"
judge_case "fails a list that names no kernel32.dll, checked first" 1 \
  "control: the module list names no kernel32.dll, so it is not a process's list" "$shell"
judge_case "fails a list that does not name the extension" 1 "extension: the module list does not name $extension" \
  "$shell" "$kernel32" 'C:\other\redshift.duckdb_extension'
# Each driver set and driver manager file, named here rather than read from the tool so that a
# file dropped from its list fails a case, and in upper case, as a module list may name it.
for file in rsodbc.dll crypto.dll ssl.dll krb5_64.dll gssapi64.dll comerr64.dll k5sprt64.dll \
  wshelp64.dll msvcr100.dll odbc32.dll odbccp32.dll; do
  module='C:\cache\'"$(tr '[:lower:]' '[:upper:]' <<<"$file")"
  judge_case "fails a list that names $file, in any case" 1 "driver: the process loaded $module" \
    "$shell" "$kernel32" "$extension" "$module"
done
judge_case "fails a module of any name inside a driver directory" 1 \
  'driver: the process loaded C:\x\.redshift-driver\abc\other.dll' "$shell" "$kernel32" "$extension" \
  'C:\x\.redshift-driver\abc\other.dll'
mkdir "$tmp/extension/.redshift-driver"
judge_case "fails a driver directory beside the extension" 1 \
  "created: the run created or changed $tmp/extension/.redshift-driver" \
  "$shell" "$kernel32" "$extension"
rmdir "$tmp/extension/.redshift-driver"
mkdir "$tmp/cache/.redshift-driver"
judge_case "fails a driver directory in the per-user cache" 1 \
  "created: the run created or changed $tmp/cache/.redshift-driver" \
  "$shell" "$kernel32" "$extension"
rmdir "$tmp/cache/.redshift-driver"
# An extension named without a folder lies in the current directory, which is watched instead.
mkdir -p "$tmp/cwd/.redshift-driver"
printf '%s\r\n' "$shell" "$kernel32" redshift.duckdb_extension > "$tmp/bare-modules.txt"
printf '%s\n' "$shell_output" > "$tmp/bare-output.txt"
out="$(cd "$tmp/cwd" && python3 -I "$tool" judge redshift.duckdb_extension "$tmp/bare-modules.txt" \
  "$tmp/cache" "$tmp/bare-output.txt" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "created: the run created or changed ./.redshift-driver" ]]; then
  pass "fails a driver directory in the current directory for an extension named without a folder"
else fail "fails a driver directory in the current directory for a bare extension name (exit $status; $out)"; fi
ln -s nowhere "$tmp/cache/.redshift-driver"
judge_case "fails a link named as a driver directory, even one that leads nowhere" 1 \
  "created: the run created or changed $tmp/cache/.redshift-driver" "$shell" "$kernel32" "$extension"
rm "$tmp/cache/.redshift-driver"
# A directory that cannot be searched hides a driver directory's size and last-write time, and one
# that cannot be read hides its entries; each fails closed. Root reads both anyway, so the cases run
# only for another user.
if [[ "$(id -u)" != 0 ]]; then
  mkdir -p "$tmp/locked/.redshift-driver"
  chmod 0600 "$tmp/locked"
  saved="$extension"; extension="$tmp/locked/redshift.duckdb_extension"
  judge_case "fails a driver directory it cannot inspect" 1 \
    "created: cannot inspect $tmp/locked/.redshift-driver: Permission denied" "$shell" "$kernel32" "$extension"
  chmod 0700 "$tmp/locked"; chmod 0300 "$tmp/locked/.redshift-driver"
  judge_case "fails a driver directory whose entries it cannot list" 1 \
    "created: cannot inspect $tmp/locked/.redshift-driver: Permission denied" "$shell" "$kernel32" "$extension"
  chmod 0700 "$tmp/locked/.redshift-driver"; rmdir "$tmp/locked/.redshift-driver"
  : > "$tmp/locked/roots.pem"; chmod 0600 "$tmp/locked"
  judge_case "fails a .pem file whose size and last-write time it cannot read" 1 \
    "created: cannot inspect $tmp/locked/roots.pem: Permission denied" "$shell" "$kernel32" "$extension"
  extension="$saved"
  chmod 0700 "$tmp/locked"
else
  for name in "fails a driver directory it cannot inspect" "fails a driver directory whose entries it cannot list" \
    "fails a .pem file whose size and last-write time it cannot read"; do
    n=$((n+1)); echo "ok $n - $name # SKIP root reads any directory"
  done
fi
# A Windows extension path's directory, read off Windows, is not the current directory.
mkdir -p "$tmp/cwd/.redshift-driver"
saved="$extension"; extension='C:\x\redshift.duckdb_extension'
cd "$tmp/cwd"
judge_case "looks beside a Windows extension path, not in the current directory" 0 "$ok" "$shell" "$kernel32" \
  "$extension"
cd "$here"; extension="$saved"
mkdir "$tmp/extension/Trust"
judge_case "fails a trust directory beside the extension, in any case" 1 \
  "created: the run created or changed $tmp/extension/Trust" \
  "$shell" "$kernel32" "$extension"
rmdir "$tmp/extension/Trust"
mkdir -p "$tmp/cache/a/trust"; : > "$tmp/cache/a/trust/roots"
judge_case "fails a trust directory deeper in the per-user cache, and each entry inside it" 1 \
  "created: the run created or changed $tmp/cache/a/trust, $tmp/cache/a/trust/roots" "$shell" "$kernel32" "$extension"
rm -r "$tmp/cache/a"
mkdir -p "$tmp/cache/b"; : > "$tmp/cache/b/ROOTS.PEM"
judge_case "fails a .pem file anywhere in the per-user cache, in any case" 1 \
  "created: the run created or changed $tmp/cache/b/ROOTS.PEM" "$shell" "$kernel32" "$extension"
rm -r "$tmp/cache/b"
: > "$tmp/extension/roots.pem.txt"
judge_case "passes a file whose name only holds .pem" 0 "$ok" "$shell" "$kernel32" "$extension"
rm "$tmp/extension/roots.pem.txt"
# An entry removed between the listing and its read holds nothing, as a vanished directory does.
mkdir -p "$tmp/cache/gone/trust"
if python3 -I -c 'import sys; sys.dont_write_bytecode = True; sys.path.insert(0, sys.argv[1]); import os, windows_modules
real = os.lstat
def lstat(path):
    if path.endswith("trust"):
        raise FileNotFoundError(2, "removed", path)
    return real(path)
os.lstat = lstat
sys.exit(windows_modules.watched(sys.argv[2], sys.argv[3]) != {})' "$here" "$extension" "$tmp/cache"; then
  pass "skips an entry removed between the listing and its read"
else fail "skips an entry removed between the listing and its read"; fi
rm -r "$tmp/cache/gone"

# The connect rule: the output must show that a connect reached the driver source and was refused
# NOT_BUILT. Windows has a trust resolver, so the refusal a build without one gives fails the rule.
no_not_built="connect: the shell's output holds no NOT_BUILT refusal for windows_amd64, so no \
connect reached the driver source"
shell_output="IO Error: $trust_message"
judge_case "fails a shell whose connect got only the refusal for a build without a trust resolver" \
  1 "$no_not_built" "$shell" "$kernel32" "$extension"
shell_output=$'\xff '"$not_built"
judge_case "reads a shell output that is not UTF-8" 0 "$ok" "$shell" "$kernel32" "$extension"
# An ATTACH failing on an option, before any connect.
attach_error="Invalid Configuration Error: Cannot attach rs: Invalid ATTACH option 'HOST'"
shell_output="$attach_error"
judge_case "fails a shell whose output holds no connect refusal" 1 "$no_not_built" \
  "$shell" "$kernel32" "$extension"
shell_output="${not_built/windows_amd64/windows_amd64_mingw}"
judge_case "fails a NOT_BUILT refusal that names another platform" 1 "$no_not_built" \
  "$shell" "$kernel32" "$extension"
shell_output="$not_built"
# The refusal the connect rule looks for is still the shipped NOT_BUILT one, not the trust refusal,
# and reaches the shell through the connect's driver lookup.
if python3 -I -c 'import sys; sys.dont_write_bytecode = True; sys.path.insert(0, sys.argv[1]); import windows_modules
needle = windows_modules.CONNECT_REFUSAL
sys.exit(not (needle in sys.argv[2] and needle not in sys.argv[3]))' \
  "$here" "$not_built_message" "$trust_message" &&
  grep -qF '"the Redshift driver could not be loaded: " + result.error.message' \
    "$repo/src/transport/odbc/odbc_process_state.cpp"; then
  pass "looks for the refusal driver_not_built.cpp gives, and not trust_store_unsupported.cpp's"
else fail "looks for the refusal driver_not_built.cpp gives, and not trust_store_unsupported.cpp's \
(NOT_BUILT: $not_built_message; trust: $trust_message)"; fi

# check, with a stand-in shell that reads the statements as DuckDB's shell does, a line at a time,
# and a stand-in for Windows that lists kernel32.dll and the extension once the marker exists. The
# stand-in gets the connect's refusal at the first redshift_query, where DuckDB's shell got it before
# ATTACH opened a connection (DuckDB's now gets it at the ATTACH); either way it prints $shell_output
# at the failed statement and exits 1, that statement's status, and exits 0 when no statement failed. Its modes: ok creates each path in $plant (":"-separated, a
# directory when it ends in "/"; a path ending in "+" gains a byte and one ending in "@" a later
# last-write time, each keeping the other, and one ending in "-" is removed), then the marker; early
# exits 3 after one line, printing $shell_output first when it is set; silent never creates the
# marker; stuck does not exit once its input closes. It prints the output to stderr, as DuckDB's
# shell prints errors. With HOST_ERROR set, the stand-in for Windows raises an OSError with that
# message instead of listing the modules.
cat > "$tmp/shell.py" <<'EOF'
import os, sys, time
mode = sys.argv[1]
if mode == "early":
    sys.stdin.readline()
    sys.stderr.write(os.environ.get("SHELL_OUTPUT", ""))
    sys.exit(3)
failed = False
for line in sys.stdin:
    if line.startswith("SELECT * FROM redshift_query("):
        sys.stderr.buffer.write(os.environb[b"SHELL_OUTPUT"] + b"\n")
        sys.stderr.flush()
        failed = True
    if mode != "silent" and line.startswith("COPY (SELECT 1) TO '"):
        literal, rest = [], line[len("COPY (SELECT 1) TO '"):]
        while not rest.startswith("'") or rest.startswith("''"):
            literal.append(rest[0])
            rest = rest[2:] if rest.startswith("''") else rest[1:]
        for path in filter(None, os.environ.get("PLANT", "").split(":")):
            if path[-1] == "-":
                os.remove(path[:-1])
            elif path[-1] in "+@":
                path, grow, status = path[:-1], path[-1] == "+", os.stat(path[:-1])
                if grow:
                    open(path, "a").write("x")
                later = status.st_mtime_ns + (0 if grow else 2 * 10**9)
                os.utime(path, ns=(status.st_atime_ns, later))
            else:
                os.makedirs(path) if path.endswith("/") else open(path, "w").close()
        open("".join(literal), "w").write("1\n")
if mode == "stuck":
    time.sleep(60)
sys.exit(1 if failed else 0)
EOF
# The check's statements: the LOAD, an ATTACH with options and, as in
# third_party/redshift_odbc/tools/connect.sql, a redshift_query on it, whose connect is refused.
# attach.sql stops after the ATTACH.
cat > "$tmp/attach.sql" <<'EOF'
LOAD redshift;
ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain,
  WORKGROUP 'analytics', ACCOUNT_ID '012345678901');
EOF
{ cat "$tmp/attach.sql"; echo "SELECT * FROM redshift_query('rs', 'SELECT 1');"; } \
  > "$tmp/stage1.sql"
harness='import sys
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import errno, glob, os, windows_modules
class Host(object):
    cache = sys.argv[4]
    def modules(self, pid):
        if not glob.glob(os.path.join(glob.escape(os.environ["TMPDIR"]), "*", "marker.csv")):
            raise windows_modules.Failure("host: the modules were listed before the statements ran")
        if os.environ.get("HOST_ERROR"):
            raise OSError(errno.EIO, os.environ["HOST_ERROR"])
        return ["C:\\Windows\\System32\\KERNEL32.DLL", os.path.abspath(sys.argv[2])]
windows_modules.MARKER_SECONDS, windows_modules.EXIT_SECONDS = int(sys.argv[5]), int(sys.argv[6])
try:
    windows_modules.check(sys.argv[2], sys.argv[3], sys.argv[7:], Host())
except windows_modules.Failure as e:
    sys.exit("%s" % e)'
# check_case <name> <mode> <marker seconds> <exit seconds> <expected-exit> <expected-output>: runs
# check on $sql with the stand-in shell in <mode>, or with $shell_command when it is set, its work
# directory under a new directory in $work, from $tmp. A failure must name a kept directory that
# holds the shell's output; a pass must leave none.
check_case() {
  local name="$1" dir="$work/$((n + 1))" out status kept; mkdir -p "$dir"
  out="$(cd "$tmp" && TMPDIR="$dir" SHELL_OUTPUT="$shell_output" PLANT="${plant:-}" bounded python3 -I -c "$harness" "$here" "$extension" \
    "$sql" "$tmp/cache" "$3" "$4" ${shell_command:-python3 -I "$tmp/shell.py" "$2"} 2>&1)" &&
    status=0 || status=$?
  kept="${out##*"(the shell's output is in "}"; kept="${kept%")"}"
  if [[ "$status" == "$5" && "$5" == 0 && "$out" == "$6" && -z "$(ls -A "$dir")" ]] ||
    [[ "$status" == "$5" && "$5" != 0 && "$out" == "$6 (the shell's output is in $dir/"*")" &&
      -f "$kept/output.txt" ]]; then pass "$name"
  else fail "$name (exit $status, wanted $5; output: $out)"; fi
}
work="$tmp/work"; sql="$tmp/stage1.sql"
check_case "check passes a shell whose connect was refused, and removes its work directory" ok 10 10 0 "$ok"
check_case "check fails a shell that exits before the statements ran" early 10 10 1 \
  "shell: it exited (3) before the statements ran"
check_case "check fails a shell whose statements do not finish in time" silent 1 10 1 \
  "shell: the statements did not finish within 1 seconds"
check_case "check fails a shell that does not exit once its input closes" stuck 10 1 1 \
  "shell: it did not exit within 1 seconds of its input closing"
sql="$tmp/attach.sql"
check_case "check fails statements that stop at the ATTACH, where the stand-in connects nothing, so exit 0" \
  ok 10 10 1 "shell: it exited with status 0x0, not 1, the status after a failed statement"
sql="$tmp/stage1.sql"
shell_output="$attach_error"
check_case "check fails a shell whose output fails a rule, naming the rule" ok 10 10 1 \
  "$no_not_built"
shell_output=$'\xff '"$not_built"
check_case "check reads a shell output that is not UTF-8" ok 10 10 0 "$ok"
shell_output="$not_built"
shell_command="$tmp/no-such-shell"
check_case "check fails a shell it cannot start" ok 10 10 1 \
  "shell: cannot start $tmp/no-such-shell: No such file or directory"
shell_command=""
export HOST_ERROR="the stand-in cannot list the modules"
check_case "check fails an OSError met once the shell has started, as its own failure" ok 10 10 1 \
  "shell: [Errno 5] the stand-in cannot list the modules"
unset HOST_ERROR
extension="extension/redshift.duckdb_extension"
check_case "check reads a relative extension path from the current directory" ok 10 10 0 "$ok"
extension="$tmp/extension/redshift.duckdb_extension"
work="$tmp/it's"
check_case "check quotes a quote in the marker's path" ok 10 10 0 "$ok"
work="$tmp/work"
# The host may be shared with tests that make these, so what was there before the run passes.
mkdir -p "$tmp/cache/.redshift-driver/set" "$tmp/cache/Trust" "$tmp/extension/trust"
: > "$tmp/cache/Trust/roots.pem"; : > "$tmp/cache/old.pem"
# Dated in the past, so the entry a case plants inside it changes its last-write time.
touch -t 200001010000 "$tmp/cache/Trust"
check_case "check passes driver and trust directories and .pem files that were there before the run" ok 10 10 0 "$ok"
plant="$tmp/cache/Trust/new"
check_case "check fails a file the run writes inside a trust directory that was there before, in any case" ok 10 10 1 \
  "created: the run created or changed $tmp/cache/Trust, $tmp/cache/Trust/new"
rm "$tmp/cache/Trust/new"
plant="$tmp/cache/Trust/roots.pem+"
check_case "check fails a file there before the run whose size changes, at the same last-write time" ok 10 10 1 \
  "created: the run created or changed $tmp/cache/Trust/roots.pem"
plant="$tmp/cache/old.pem@"
check_case "check fails a file there before the run whose last-write time changes, at the same size" ok 10 10 1 \
  "created: the run created or changed $tmp/cache/old.pem"
plant="$tmp/cache/old.pem-"
check_case "check fails a .pem file there before the run that the run removes" ok 10 10 1 \
  "removed: the run removed $tmp/cache/old.pem"
: > "$tmp/cache/old.pem"
rm -r "$tmp/cache/.redshift-driver" "$tmp/cache/Trust" "$tmp/extension/trust" "$tmp/cache/old.pem"
plant="$tmp/cache/trust/"
check_case "check fails a trust directory planted in the per-user cache during the run" ok 10 10 1 \
  "created: the run created or changed $tmp/cache/trust"
rmdir "$tmp/cache/trust"
plant="$tmp/extension/ca.pem:$tmp/extension/.redshift-driver/"
check_case "check fails what the run creates beside the extension, naming each" ok 10 10 1 \
  "created: the run created or changed $tmp/extension/.redshift-driver, $tmp/extension/ca.pem"
rm -r "$tmp/extension/ca.pem" "$tmp/extension/.redshift-driver"
plant=""
out="$(REDSHIFT_DRIVER_DIR="$tmp" python3 -I "$tool" check "$extension" "$tmp/stage1.sql" duckdb 2>&1)" &&
  status=0 || status=$?
if [[ "$status" == 1 && "$out" == "directory: REDSHIFT_DRIVER_DIR is set, so a driver directory could be made"* ]]
then pass "check refuses to run while REDSHIFT_DRIVER_DIR is set"
else fail "check refuses to run while REDSHIFT_DRIVER_DIR is set (exit $status; $out)"; fi

# modules, with the same stand-ins: it writes the module list, a path per line, and the shell's
# output, and judges neither; a shell that exits with another status fails as check fails it.
modules_harness='import sys
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import windows_modules
class Host(object):
    def modules(self, pid):
        return ["C:\\Windows\\System32\\KERNEL32.DLL", "C:\\set\\abcdefghijk23456.dll"]
try:
    windows_modules.shell_modules(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:], Host())
except windows_modules.Failure as e:
    sys.exit("%s" % e)'
# modules_case <name> <mode> <expected-exit> <expected-output>: runs modules on $sql. On a pass, the
# list and the output must be what the stand-ins gave, and the work directory removed; a failure
# must keep it.
listed=$'C:\\Windows\\System32\\KERNEL32.DLL\nC:\\set\\abcdefghijk23456.dll'
modules_case() {
  local dir="$work/$((n + 1))" out status; mkdir -p "$dir"
  rm -f "$tmp/listed.txt" "$tmp/shell-output.txt"
  out="$(cd "$tmp" && TMPDIR="$dir" SHELL_OUTPUT="Binder Error: no connect" bounded python3 -I -c \
    "$modules_harness" "$here" "$tmp/listed.txt" "$tmp/shell-output.txt" "$sql" \
    python3 -I "$tmp/shell.py" "$2" 2>&1)" && status=0 || status=$?
  if [[ "$status" == 0 && "$3" == 0 && -z "$out" && -z "$(ls -A "$dir")" &&
    "$(cat "$tmp/listed.txt")" == "$listed" &&
    "$(cat "$tmp/shell-output.txt")" == "Binder Error: no connect" ]] ||
    [[ "$status" == "$3" && "$3" != 0 && "$out" == "$4 (the shell's output is in $dir/"*")" ]]; then
    pass "$1"
  else fail "$1 (exit $status, wanted $3; output: $out)"; fi
}
modules_case "modules writes the module list and the shell's output without judging them" ok 0 ""
first_sql="$sql"
sql="$tmp/attach.sql"
modules_case "modules fails a shell that exits with a status other than a failed statement's" \
  ok 1 \
  "shell: it exited with status 0x0, not 1, the status after a failed statement"
modules_case "modules fails a shell that exits before the statements ran" early 1 \
  "shell: it exited (3) before the statements ran"
if [[ "$(cat "$tmp/shell-output.txt")" == "Binder Error: no connect" ]]
then pass "modules still writes the output of a shell whose run failed"
else fail "modules still writes the output of a shell whose run failed"; fi
sql="$tmp/stage1.sql"
out="$(cd "$tmp" && TMPDIR="$work" python3 -I -c "$modules_harness" "$here" "$tmp/listed.txt" \
  "$tmp/shell-output.txt" "$tmp/stage1.sql" "$tmp/no-such-shell" 2>&1)" && status=0 || status=$?
missing="shell: cannot start $tmp/no-such-shell: No such file or directory"
if [[ "$status" == 1 && "$out" == "$missing (the shell's output is in $work/"*")" ]]
then pass "modules fails a shell it cannot start, naming the error"
else fail "modules fails a shell it cannot start, naming the error (exit $status; $out)"; fi
out="$(cd "$tmp" && TMPDIR="$work" SHELL_OUTPUT="Binder Error: no connect" python3 -I -c \
  "$modules_harness" "$here" "$tmp/no-dir/listed.txt" "$tmp/shell-output.txt" "$first_sql" \
  python3 -I "$tmp/shell.py" ok 2>&1)" && status=0 || status=$?
unwritable="shell: [Errno 2] No such file or directory: '$tmp/no-dir/listed.txt'"
if [[ "$status" == 1 && "$out" == "$unwritable (the shell's output is in $work/"*")" ]]
then pass "modules fails a module list it cannot write, naming the error and the kept output"
else fail "modules fails a module list it cannot write, naming the kept output (exit $status; $out)"; fi
# A failed run whose output cannot be copied is reported as the run's failure, not the copy's.
out="$(cd "$tmp" && TMPDIR="$work" SHELL_OUTPUT="Binder Error: no connect" python3 -I -c \
  "$modules_harness" "$here" "$tmp/listed.txt" "$tmp/no-dir/shell-output.txt" "$sql" \
  python3 -I "$tmp/shell.py" early 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "shell: it exited (3) before the statements ran (the shell's output is in $work/"*")" ]]
then pass "modules reports a failed run whose output it cannot copy as the run's failure"
else fail "modules reports a failed run whose output it cannot copy as the run's failure (exit $status; $out)"; fi
out="$(cd "$tmp" && TMPDIR="$work" SHELL_OUTPUT="Binder Error: no connect" python3 -I -c \
  "$modules_harness" "$here" "$tmp/listed.txt" "$tmp/no-dir/shell-output.txt" "$first_sql" \
  python3 -I "$tmp/shell.py" ok 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "shell: [Errno 2] No such file or directory: '$tmp/no-dir/shell-output.txt'"* ]]
then pass "modules fails a passed run whose output it cannot copy"
else fail "modules fails a passed run whose output it cannot copy (exit $status; $out)"; fi

# refuses_off_windows <name> <argument>...: the command fails with the Windows-only message.
refuses_off_windows() {
  local name="$1" out status; shift
  out="$(python3 -I "$tool" "$@" 2>&1)" && status=0 || status=$?
  if [[ "$status" == 1 && "$out" == "windows: this command runs on Windows only" ]]; then pass "$name"
  else fail "$name (exit $status; $out)"; fi
}
refuses_off_windows "self-test refuses to run off Windows" self-test
refuses_off_windows "check refuses to run off Windows" check "$extension" "$tmp/modules.txt" duckdb
refuses_off_windows "modules refuses to run off Windows" modules "$tmp/listed.txt" "$tmp/out.txt" \
  "$tmp/stage1.sql" duckdb
refuses_off_windows "folders refuses to run off Windows" folders

# run-windows-loading.sh, which runs the self-test among the Windows-only suites: it refuses another
# host, and, under a stand-in uname that names Git's bash, a suite that fails (here the self-test,
# off Windows) fails the run, naming it.
runner="$here/run-windows-loading.sh"
out="$(bash "$runner" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out" == "run-windows-loading: runs on Windows, from Git's bash, not on $(uname -s)" ]]; then
  pass "run-windows-loading.sh refuses a host that is not Windows"
else fail "run-windows-loading.sh refuses a host that is not Windows (exit $status; $out)"; fi
mkdir -p "$tmp/git-bash"; printf '#!/bin/sh\necho MINGW64_NT-10.0-20348\n' > "$tmp/git-bash/uname"; chmod +x "$tmp/git-bash/uname"
out="$(PATH="$tmp/git-bash:$PATH" bash "$runner" 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "${out##*$'\n'}" == "run-windows-loading: "*" failed: the module-list helper's own cases"* ]]
then
  pass "run-windows-loading.sh fails when a runner-only suite fails, naming it"
else fail "run-windows-loading.sh fails when a suite fails (exit $status; $out)"; fi

out="$(python3 -I "$tool" judge "$extension" "$tmp/missing.txt" "$tmp/cache" "$tmp/output.txt" 2>&1)" &&
  status=0 || status=$?
if [[ "$status" == 1 && "$out" == "file: [Errno 2] No such file or directory: '$tmp/missing.txt'" ]]; then
  pass "judge refuses a module list it cannot read"
else fail "judge refuses a module list it cannot read (exit $status; $out)"; fi
out="$(python3 -I "$tool" bogus 2>&1)" && status=0 || status=$?
if [[ "$status" == 2 && "$out" == "usage: windows_modules.py self-test"* ]]; then
  pass "prints its usage for another command"
else fail "prints its usage for another command (exit $status; $out)"; fi

# The Win32 calls, through stand-ins for ctypes' Windows calls, which no Linux case can make:
# OpenProcess, EnumProcessModules and GetModuleFileNameExW failing, a module list that grows once,
# one that gains a module at each read and one that keeps growing, each counting its reads, and the
# per-user cache's known folder: absolute, relative, null, or failed with or without a path, and
# GetTempPathW and GetLongPathNameW: a short temp path made long, or either call failing.
# stub_case <name> <case> <expected>: the case's output, run through bounded, must equal <expected>.
stubs='import sys, types
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import ctypes, windows_modules
class Call(object):
    """A Windows function: callable, with argtypes and restype to set."""
    def __init__(self, body):
        self.body = body
    def __call__(self, *args):
        return self.body(*args)
fake = types.SimpleNamespace(**{n: getattr(ctypes, n) for n in dir(ctypes) if n[:2] != "__"})
fake.get_last_error = lambda: 5
wintypes = types.SimpleNamespace(DWORD=ctypes.c_uint32, WORD=ctypes.c_uint16, BOOL=ctypes.c_int,
                                 HANDLE=ctypes.c_void_p, HMODULE=ctypes.c_void_p,
                                 LPWSTR=ctypes.c_wchar_p, LPCWSTR=ctypes.c_wchar_p)
kernel32 = types.SimpleNamespace()
windows_modules.win32 = lambda: (fake, wintypes, kernel32)
freed = []
def known_folder(result, text):
    folder = ctypes.create_unicode_buffer(text) if text is not None else None
    def call(_, flags, token, out):
        out._obj.value = ctypes.addressof(folder) if folder is not None else None
        return result
    dlls = {"shell32": types.SimpleNamespace(SHGetKnownFolderPath=Call(call)),
            "ole32": types.SimpleNamespace(CoTaskMemFree=Call(lambda p: freed.append(p.value)))}
    fake.WinDLL = lambda name, **_: dlls[name]
    return folder
def temp_paths(case):
    # GetTempPathW gives an 8.3 path with its final separator; GetLongPathNameW expands what it gets.
    def temp(size, path):
        path.value = "C:\\Users\\RUNNER~1\\AppData\\Local\\Temp\\"
        return 0 if case == "temp-failed" else len(path.value)
    def long_path(short, path, size):
        path.value = short.replace("RUNNER~1", "runneradmin")
        return {"long-failed": 0, "long-truncated": size}.get(case, len(path.value))
    kernel32.GetTempPathW, kernel32.GetLongPathNameW = Call(temp), Call(long_path)
case = sys.argv[2]
try:
    if case == "open":
        kernel32.OpenProcess = lambda access, inherit, pid: None
        windows_modules.Windows.__new__(windows_modules.Windows).modules(1234)
    elif case in ("grow", "regrow", "creep", "enum", "name"):
        # Two modules, except in "creep", which has one more at each read and is read first with
        # room for one, so that the next read holds them only with the slack added to its room.
        # "grow" reports one more than each read has room for, and "regrow" does so on the first
        # read only, as a list that grew between calls.
        reads, width = [], ctypes.sizeof(ctypes.c_void_p)
        def enumerate_modules(handle, modules, size, needed):
            reads.append(size)
            if case == "enum":
                return 0
            if case == "grow" or (case == "regrow" and len(reads) == 1):
                needed._obj.value = size + width
                return 1
            count = 1 + len(reads) if case == "creep" else 2
            for i in range(min(count, size // width)):
                modules[i] = 0x10 * (i + 1)
            needed._obj.value = count * width
            return 1
        def file_name(handle, module, path, size):
            path.value = "C:\\Windows\\System32\\m%x.dll" % module
            return 0 if case == "name" else len(path.value)
        kernel32.K32EnumProcessModules = enumerate_modules
        kernel32.K32GetModuleFileNameExW = file_name
        try:
            print("\n".join(windows_modules.list_modules(1, *([1] if case == "creep" else []))))
        finally:
            print("reads: %d" % len(reads))
    elif case in ("temp", "temp-failed", "long-failed", "long-truncated"):
        temp_paths(case)
        print(windows_modules.temp_folder())
    elif case == "folders":
        temp_paths(case)
        known_folder(0, "C:\\Users\\u\\AppData\\Local")
        print("status: %d" % windows_modules.main(["windows_modules.py", "folders"]))
    else:
        result, text = {"relative": (0, "AppData\\Local"), "failed": (-2147024894, None),
                        "absolute": (0, "C:\\Users\\u\\AppData\\Local"), "null": (0, None),
                        "failed-path": (-2147024894, "C:\\Users\\u\\AppData\\Local")}[case]
        folder = known_folder(result, text)
        print(windows_modules.per_user_cache())
        print("freed" if len(freed) == 1 else "not freed")
except windows_modules.Failure as e:
    print(e)
    print("freed" if len(freed) == 1 else "not freed")'
stub_case() {
  local out
  out="$(bounded python3 -I -c "$stubs" "$here" "$2" 2>&1)" || true
  if [[ "$out" == "$3" ]]; then pass "$1"; else fail "$1 (output: $out)"; fi
}
stub_case "fails when OpenProcess cannot open the shell, naming the call and its error" open \
  "enumerate: OpenProcess failed for the shell (Windows error 5)
not freed"
stub_case "fails a module list that keeps growing, after 8 reads" grow \
  "reads: 8
enumerate: EnumProcessModules kept reporting a longer list
not freed"
stub_case "reads a module list again, with room for it, when it grew since the first read" regrow \
  'C:\Windows\System32\m10.dll
C:\Windows\System32\m20.dll
reads: 2'
stub_case "reads a module list that gains a module at each read in one more read" creep \
  'C:\Windows\System32\m10.dll
C:\Windows\System32\m20.dll
C:\Windows\System32\m30.dll
reads: 2'
stub_case "fails when EnumProcessModules fails, naming the call and its error" enum \
  "reads: 1
enumerate: EnumProcessModules failed (Windows error 5)
not freed"
stub_case "fails when GetModuleFileNameExW fails, naming the call and its error" name \
  "reads: 1
enumerate: GetModuleFileNameExW failed (Windows error 5)
not freed"
stub_case "fails a per-user cache whose known folder is not absolute, and frees it" relative \
  "directory: SHGetKnownFolderPath(FOLDERID_LocalAppData) found no folder (0x00000000)
freed"
stub_case "fails a per-user cache whose known folder cannot be found, naming the result" failed \
  "directory: SHGetKnownFolderPath(FOLDERID_LocalAppData) found no folder (0x80070002)
freed"
stub_case "fails a per-user cache whose known folder call fails but gives a path, and frees it" \
  failed-path "directory: SHGetKnownFolderPath(FOLDERID_LocalAppData) found no folder (0x80070002)
freed"
stub_case "fails a per-user cache whose known folder call succeeds with a null path, and frees it" \
  null "directory: SHGetKnownFolderPath(FOLDERID_LocalAppData) found no folder (0x00000000)
freed"
stub_case "puts the per-user cache under the known folder, and frees it" absolute \
  'C:\Users\u\AppData\Local\duckdb-redshift
freed'
stub_case "gives the temp folder's long form, without its final separator" temp \
  'C:\Users\runneradmin\AppData\Local\Temp'
stub_case "fails when GetTempPathW fails, naming the call and its error" temp-failed \
  "directory: GetTempPathW failed (Windows error 5)
not freed"
stub_case "fails when GetLongPathNameW fails, naming the call and its error" long-failed \
  "directory: GetLongPathNameW failed (Windows error 5)
not freed"
stub_case "fails a long temp path that does not fit its buffer" long-truncated \
  "directory: GetLongPathNameW failed (Windows error 5)
not freed"
stub_case "folders prints the long temp folder, then the per-user cache" folders \
  'C:\Users\runneradmin\AppData\Local\Temp
C:\Users\u\AppData\Local\duckdb-redshift
status: 0'

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
