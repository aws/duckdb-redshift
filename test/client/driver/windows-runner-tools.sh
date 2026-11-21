#!/usr/bin/env bash
# The tool cases only a Windows host can run, one step per call, each a suite of
# run-windows-loading.sh, which make test_driver runs, so every such run repeats them, except
# admin-image in a logon that is not interactive (exit 3 below):
#
#   bash windows-runner-tools.sh downloads
#   bash windows-runner-tools.sh admin-image|authenticode|written-bytes <build dir>
#   bash windows-runner-tools.sh exports|imports-negative|bare-names <build dir>
#
# downloads      lib/download.py over this host's sockets, through a connected socket pair
#                whose other end answers as a server would: a body within the bound downloads
#                with its SHA-256; a declared or streamed body over the bound, a stalled body
#                (at a 2-second read timeout) and a redirect fail and leave no file; an
#                http:// URL is refused.
# admin-image    the lock's MSI, copied from RS_DRIVER_MIRROR or the build's kept copy, must
#                have the lock's SHA-256 before msiexec /a /qn extracts it into a new, empty
#                directory, by a non-elevated account in an interactive logon (a logon that is not
#                interactive skips, elevated or not; an interactive one that is elevated fails);
#                then lib/msi.py
#                compare checks the image against the cabinet, and each set file's SHA-256 there
#                must be its lock row's source-sha256.
# authenticode   the build's renamed msvcr100.dll, with the lock's sha256, is Valid for
#                Get-AuthenticodeSignature: renaming leaves Microsoft's signature intact.
# written-bytes  the loadable extension's RCDATA resources, the bytes its loader writes, are
#                the set the lock names, file by file, with the lock's sha256 and nothing else.
# exports        the loadable extension's export table holds the name DuckDB's loader looks up,
#                redshift_duckdb_cpp_init, holds no loader, driver or set name and no
#                nameless export, and forwards nothing: the Windows form of
#                only-embedded_test.sh's Linux rule "the loadable extension exports no loader
#                symbol". A Windows build exports DuckDB's own API too, which its headers export
#                and the loadable links statically.
# imports-negative
#                the negative control of the interface suite's import-table case: a synthetic PE
#                written at test time, which imports KERNEL32.dll (the case's own control) and
#                ODBC32.dll, handed to the client suite's case through RS_IMPORTS_DLL with
#                RS_REQUIRE_DUMPBIN=1, must fail it on that import: the failure must hold the
#                import check's own expression, and the planted file's name holds no odbc32, so no
#                other failure passes this step. The case's passing run is the test_driver
#                recipe's, against this build's loadable extension.
# bare-names     the strings the rename wrote over the bare-name loads, read from the lock: with
#                a fake planted under each original name in a folder of the run's own, which the
#                control loads from there, a LoadLibraryA of each rewritten string maps nothing
#                (ERROR_INVALID_NAME, or ERROR_MOD_NOT_FOUND, which Windows Server 2022 gives), and
#                no file of that name can be created there, so no planted file can ever answer a
#                rewritten bare-name load (windows_bare_names.py, whose rules run on Linux too).
#
# The build dir is a windows_amd64 embedded build: its extension directory holds the lock it
# embedded (redshift_driver.lock) and the fetched set (redshift_driver/). Windows programs get
# Windows paths, in long form, and no MSYS argument conversion. Exits 1 naming the step's rule,
# 3 when admin-image cannot run msiexec /a because the logon is not interactive, or 2 on bad usage.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools="$here/../../../third_party/redshift_odbc/tools"
step="${1:-}"
fail() {
  echo "windows-runner-tools: $step: $*" >&2
  exit 1
}
# A session that cannot run the step: run-windows-loading.sh reports it as a skip, not a failure.
cannot_run_here() {
  echo "windows-runner-tools: $step: $*" >&2
  exit 3
}
usage() {
  echo "usage: windows-runner-tools.sh downloads |" \
    "admin-image|authenticode|written-bytes|exports|imports-negative|bare-names <build dir>" >&2
  exit 2
}
windows_path() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w -l "$1"; else printf '%s\n' "$1"; fi
}
# A Windows path as this shell reads it; without cygpath (off Windows, in a test) it is taken as is.
posix_path() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s\n' "$1"; fi
}
# Runs a Windows program with its arguments as given: MSYS would turn /a into a path.
windows_program() { MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$@"; }
# Python's output on Windows ends its lines with a carriage return, which every reading drops.
sha256() {
  python3 -I -c 'import hashlib, sys
print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$1" | tr -d '\r'
}
# The last lines of msiexec's log, which it may write in UTF-16.
log_tail() {
  python3 -I -c 'import sys
data = open(sys.argv[1], "rb").read()
text = data.decode("utf-16") if data[:2] == b"\xff\xfe" else data.decode("latin-1")
print(" | ".join(text.splitlines()[-20:]))' "$work/msiexec.log" 2>/dev/null | tr -d '\r' ||
    echo "(no log)"
}

work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT

# The build's lock, through check-lock.sh.
read_lock() {
  ext="$1/extension/redshift"
  set_dir="$ext/redshift_driver"
  lock="$(bash "$tools/check-lock.sh" "$ext/redshift_driver.lock" | tr -d '\r')" ||
    fail "the build's lock $ext/redshift_driver.lock fails check-lock.sh"
  grep -q '^driver windows_amd64 ' <<<"$lock" ||
    fail "the build's lock has no windows_amd64 driver row"
}

downloads() {
  python3 -I -B - "$tools/lib/download.py" "$work" <<'EOF'
import hashlib, http.client, importlib.util, os, socket, subprocess, sys, threading
spec = importlib.util.spec_from_file_location("download", sys.argv[1])
dl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dl)
work, failed = sys.argv[2], []

def answer(head, body=b"", stall=False):
    # The server's end: reads the request, answers, and for a stall holds the socket open.
    def serve(sock, done):
        with sock:
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = sock.recv(4096)
                if not chunk:
                    return
                request += chunk
            sock.sendall(head + b"\r\n\r\n" + body)
            if stall:
                done.wait(20)
    return serve

def run(name, serve, want):
    # want: the result's start; a failure must also leave no file behind.
    out, done, opened = os.path.join(work, name), threading.Event(), []
    def opener(host, port, timeout):
        ours, theirs = socket.socketpair()
        ours.settimeout(timeout)
        server = threading.Thread(target=serve, args=(theirs, done))
        server.daemon = True
        server.start()
        opened.append(server)
        connection = http.client.HTTPConnection(host, port or 443, timeout=timeout)
        connection.sock = ours  # a connection with a socket sends on it, and never connects
        return connection
    try:
        size = dl.download("https://h.example/a.msi", out, 10, opener)
        digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
        result = "downloaded %d bytes with SHA-256 %s" % (size, digest)
    except dl.Failure as e:
        result = "failed: %s" % e
    finally:
        done.set()
        for server in opened:
            server.join(20)
    print("%s: %s" % (name, result))
    if not result.startswith(want):
        failed.append("%s: %s, wanted %s" % (name, result, want))
    elif want.startswith("failed") and os.path.exists(out):
        failed.append("%s: %s, and left %s" % (name, result, out))

ok, body = b"HTTP/1.1 200 OK\r\n", b"12345678"
url = "downloading https://h.example/a.msi failed: "
run("within", answer(ok + b"Content-Length: 8", body),
    "downloaded 8 bytes with SHA-256 " + hashlib.sha256(body).hexdigest())
run("declared", answer(ok + b"Content-Length: 11", b"12345678901"),
    "failed: %sits declared 11 bytes are over the 10-byte bound" % url)
run("streamed", answer(ok + b"Connection: close", b"12345678901"),
    "failed: %sthe body passed the 10-byte bound and was stopped" % url)
run("redirect", answer(b"HTTP/1.1 302 Found\r\nLocation: https://h.example/b.msi\r\n"
                       b"Content-Length: 0"),
    "failed: https://h.example/a.msi answered HTTP 302; redirects are not followed")
dl.TIMEOUT = 2
run("stalled", answer(ok + b"Content-Length: 8", b"12", stall=True), "failed: %stimed out" % url)
run("short", answer(ok + b"Content-Length: 8", b"12"),
    "failed: %sthe body ended after 2 of its declared 8 bytes" % url)
plain = os.path.join(work, "plain")
cli = subprocess.run([sys.executable, "-I", sys.argv[1], "http://h.example/a.msi", plain, "10"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
told = cli.stdout.strip()
print("http: %s" % told)
if cli.returncode != 1 or "only an https:// URL" not in told or os.path.exists(plain):
    failed.append("http: exit %d, %s" % (cli.returncode, told))
if failed:
    sys.exit("windows-runner-tools: downloads: " + "; ".join(failed))
EOF
}

admin_image() {
  read_lock "$1"
  local msi_sha source="" got out
  msi_sha="$(awk '$1 == "driver" && $2 == "windows_amd64" { print $7 }' <<<"$lock")"
  for candidate in ${RS_DRIVER_MIRROR:+"$RS_DRIVER_MIRROR/$msi_sha"} \
    "$set_dir/packages/$msi_sha"; do
    if [[ -f "$candidate" ]]; then source="$candidate"; break; fi
  done
  [[ -n "$source" ]] ||
    fail "neither RS_DRIVER_MIRROR nor $set_dir/packages holds the locked MSI $msi_sha"
  # msiexec reads the copy that was hashed, so nothing can change the file between the two.
  cp -- "$source" "$work/package.msi"
  got="$(sha256 "$work/package.msi")"
  [[ "$got" == "$msi_sha" ]] ||
    fail "$source has SHA-256 $got, the lock expects $msi_sha; msiexec was not run"
  # msiexec /a runs only as a non-elevated account, into a folder only that
  # account can reach. The grant names the token's SID: a bare name could resolve to a
  # local account of the same name as the domain one.
  local answer query elevated="" sid="" groups="" logon sid_re='S-1-[0-9]+(-[0-9]+)+' groups_re
  groups_re="^$sid_re( $sid_re)*\$"
  query='$i = [Security.Principal.WindowsIdentity]::GetCurrent(); '
  query+='([Security.Principal.WindowsPrincipal]$i).IsInRole('
  query+='[Security.Principal.WindowsBuiltInRole]::Administrator); $i.User.Value; '
  query+="(\$i.Groups | ForEach-Object { \$_.Value }) -join ' '"
  answer="$(windows_program powershell -NoProfile -NonInteractive -Command "$query" |
    tr -d '\r')" ||
    fail "powershell could not say whether this account is elevated; msiexec was not run"
  { read -r elevated; read -r sid; read -r groups; } <<<"$answer" || true
  # The answer's shape first, so a broken query can never become a skip below.
  case "$elevated" in
  True | False) ;;
  *) fail "powershell answered '$elevated', not True or False, to whether this account is" \
    "elevated; msiexec was not run" ;;
  esac
  [[ "$sid" =~ ^$sid_re$ ]] ||
    fail "powershell gave '$sid', not a SID, for this account, so the image folder cannot be made" \
      "owner-only"
  # Only a well-formed answer may skip: a groups line that is empty or holds anything but SIDs
  # fails, so a broken query cannot become a skip.
  [[ -n "$groups" ]] || fail "powershell gave no groups for this token, so its logon type is" \
    "unknown; msiexec was not run"
  [[ "$groups" =~ $groups_re ]] || fail "powershell's groups line for this token," \
    "'$groups', is not a list of SIDs, so its logon type is unknown; msiexec was not run"
  # From a scheduled-task (batch) logon as a non-elevated account, the Windows Installer service
  # was observed to refuse msiexec (exit 1601, 0x80070005, note 2774), and a service logon likely
  # does the same, so only a token holding INTERACTIVE (S-1-5-4) runs it. A network logon
  # (OpenSSH, WinRM) skips too, though only batch was seen to fail. The checks above stay first,
  # so a skip never hides a wrong or missing MSI.
  # The cross-check runs in an interactive logon as a non-elevated account. A logon that is not
  # interactive skips whatever its token holds, elevated included: the account is the host's, not
  # the test's choice, and a build host may offer only an administrator (a Windows container may
  # run only as its administrator). An interactive logon that is elevated is a misuse of the check,
  # and fails.
  if [[ " $groups " != *" S-1-5-4 "* ]]; then
    logon=""
    [[ " $groups " != *" S-1-5-3 "* ]] || logon=" and holds BATCH (S-1-5-3)"
    [[ " $groups " != *" S-1-5-6 "* ]] || logon=" and holds SERVICE (S-1-5-6)"
    [[ "$elevated" != True ]] || logon+=" and is elevated"
    cannot_run_here "this logon is not interactive: its token lacks INTERACTIVE (S-1-5-4)$logon;" \
      "msiexec was not run"
  fi
  [[ "$elevated" != True ]] ||
    fail "this account is elevated (an administrator's token is in use); msiexec was not run"
  mkdir "$work/image"
  windows_program icacls "$(windows_path "$work/image")" /inheritance:r \
    /grant:r "*$sid:(OI)(CI)F" >/dev/null ||
    fail "icacls could not give the image folder an owner-only access list; msiexec was not run"
  windows_program msiexec /a "$(windows_path "$work/package.msi")" /qn \
    TARGETDIR="$(windows_path "$work/image")" "/l*v" "$(windows_path "$work/msiexec.log")" ||
    fail "msiexec /a exited $?; the end of its log: $(log_tail)"
  out="$(python3 -I "$tools/lib/msi.py" compare "$work/package.msi" "$work/image" 2>&1 |
    tr -d '\r')" || fail "msi.py compare: $out"
  while read -r _ _ name _ _ _ _ path source_sha; do
    got="$(awk -v p="$path" '$2 == p { print $1 }' <<<"$out")"
    [[ "$got" == "$source_sha" ]] || fail "$name at $path in the administrative image has" \
      "SHA-256 ${got:-(none)}, the lock expects $source_sha"
  done < <(grep -E '^(driver|library) windows_amd64 ' <<<"$lock")
  echo "windows-runner-tools: admin-image: msiexec /a extracted the locked MSI," \
    "and its set files are the lock's"
}

authenticode() {
  read_lock "$1"
  local name embedded want got file status
  read -r name embedded < <(awk '$1 == "rename" && $2 == "windows_amd64" &&
    tolower($3) == "msvcr100.dll" { print $3, $4 }' <<<"$lock") ||
    fail "the build's lock renames no msvcr100.dll"
  want="$(awk -v n="$name" '$1 == "library" && $2 == "windows_amd64" && $3 == n { print $4 }' \
    <<<"$lock")"
  file="$set_dir/$embedded"
  [[ -f "$file" ]] || fail "$file, the renamed $name, is missing"
  got="$(sha256 "$file")"
  [[ "$got" == "$want" ]] || fail "$file has SHA-256 $got, the lock expects $want"
  # The path goes through the environment, so no quoting of it reaches PowerShell's parser.
  local command='(Get-AuthenticodeSignature -LiteralPath $env:RS_AUTHENTICODE_FILE)'
  status="$(RS_AUTHENTICODE_FILE="$(windows_path "$file")" windows_program powershell \
    -NoProfile -NonInteractive -Command "$command.Status.ToString()" | tr -d '\r')" ||
    fail "Get-AuthenticodeSignature could not read $file"
  [[ "$status" == Valid ]] ||
    fail "Get-AuthenticodeSignature reports ${status:-nothing} for $file, not Valid"
  echo "windows-runner-tools: authenticode: $embedded, the renamed $name, is Valid"
}

written_bytes() {
  read_lock "$1"
  printf '%s\n' "$lock" >"$work/lock"
  python3 -I -B - "$tools/lib/pe.py" "$work/lock" "$ext/redshift.duckdb_extension" <<'EOF'
import hashlib, importlib.util, json, sys
spec = importlib.util.spec_from_file_location("redshift_odbc_pe", sys.argv[1])
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)
# Each set file by the label driver_info.py's resource script gives it: its stem in upper case.
rows = [line.split(" ") for line in open(sys.argv[2], encoding="ascii").read().splitlines()]
renames = dict((r[2], r[3]) for r in rows if r[0] == "rename" and r[1] == "windows_amd64")
want = {}
for r in rows:
    if r[0] in ("driver", "library") and r[1] == "windows_amd64":
        want[json.dumps(r[4].upper())] = (renames.get(r[2], r[2]), r[3])
    elif r[0] == "notice":
        want[json.dumps(r[3].upper())] = (r[1], r[2])
image = sys.argv[3]
try:
    data = open(image, "rb").read()
    found = {}
    for label, _, size, at in pe.rcdata(pe.Image(data)):
        found.setdefault(label, []).append(hashlib.sha256(data[at : at + size]).hexdigest())
except (OSError, pe.Failure) as e:
    sys.exit("windows-runner-tools: written-bytes: cannot read %s: %s" % (image, e))
problems = ["RCDATA %s is not a file of the lock's set" % label
            for label in found if label not in want]
for label, (name, sha) in sorted(want.items()):
    got = found.get(label, [])
    if got != [sha]:
        problems.append("%s (RCDATA %s) is %s, the lock expects %s once"
                        % (name, label, got or "missing", sha))
if problems:
    sys.exit("windows-runner-tools: written-bytes: %s: %s" % (image, "; ".join(problems)))
print("windows-runner-tools: written-bytes: %s holds the %d files of the lock's set"
      % (image, len(want)))
EOF
}

# The extension directory of a windows_amd64 build, and the loadable extension in it.
extension_dir() {
  ext="$1/extension/redshift"
  image="$ext/redshift.duckdb_extension"
  [[ -f "$image" ]] || fail "$image is missing; run make release REDSHIFT_CLIENT=odbc" \
    "REDSHIFT_DRIVER=embedded"
}

extension_exports() {
  extension_dir "$1"
  python3 -I -B - "$tools/lib/pe.py" "$image" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("redshift_odbc_pe", sys.argv[1])
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)
# DuckDB's loader looks up one entry point in a C++ extension by its undecorated name,
# <name>_duckdb_cpp_init (extension_load.cpp's LoadExternalExtensionInternal; the extension exports
# it through extension_loader.hpp's DUCKDB_CPP_EXTENSION_ENTRY inside extern "C"). A Windows build
# of a DuckDB extension also exports DuckDB's own API, which its headers mark __declspec(dllexport)
# and which the loadable links statically, so the table holds thousands of DuckDB's names besides
# the entry point, and the rule says what must not be there rather than listing what may. It is the
# Windows form of only-embedded_test.sh's Linux one, "the loadable extension exports no loader
# symbol": the entry point is there, no exported name is the loader's, the driver's or a set file's,
# and nothing is forwarded out of the image. An export with no name reads as "@<ordinal>", which
# GetProcAddress can still reach, so it is refused too.
ENTRY = "redshift_duckdb_cpp_init"
# The loader's and the driver's names, as only-embedded_test.sh refuses them on Linux, and the
# set's own file names but crypto.dll's: DuckDB's own exports hold "Crypto" (CryptoHashState), so
# that one would refuse a clean image. "ssl" matches none of them.
REFUSED = ("redshift_driver", "loadembeddeddriver", "allocateenvironmenthandle", "driverloader",
           "loaddriverfileset", "windows_driver", "rsodbc", "krb5_64", "gssapi64", "k5sprt64",
           "comerr64", "wshelp64", "msvcr100", "ssl")
path = sys.argv[2]
try:
    image = pe.Image(open(path, "rb").read())
    names = pe.export_names(image)
    _, forwarders = pe.exports(image)
except (OSError, pe.Failure) as e:
    sys.exit("windows-runner-tools: exports: cannot read %s: %s" % (path, e))
problems = []
if ENTRY not in names:
    problems.append("its export table does not hold DuckDB's entry point %s (it holds %d name%s)"
                    % (ENTRY, len(names), "" if len(names) == 1 else "s"))
for name in names:
    low = name.lower()
    if name.startswith("@"):
        problems.append("%s is exported with no name, which GetProcAddress can still reach" % name)
    elif low.startswith("sql") or any(word in low for word in REFUSED):
        problems.append("%s is exported, which is the loader's, the driver's or a set file's" % name)
problems += ["an export is forwarded to %s" % f.decode("ascii", "replace") for f in forwarders]
if problems:
    sys.exit("windows-runner-tools: exports: %s: %s" % (path, "; ".join(problems[:5])))
print("windows-runner-tools: exports: %s exports %d name%s, DuckDB's own with the entry point %s, "
      "and no loader, driver or set name" % (path, len(names), "" if len(names) == 1 else "s", ENTRY))
EOF
}

imports_negative() {
  extension_dir "$1"
  local suite="$ext/redshift_client_tests.exe" planted="$work/planted-import.dll" out status=0
  local case_name="The loadable extension imports no ODBC driver manager"
  [[ -x "$suite" ]] || fail "$suite is missing; run make test_driver, which builds the client suite"
  # A real PE32+ with an import table, written here: no compiler, and the same bytes on every host.
  python3 -I "$here/make_pe.py" "$planted" \
    '{"rdata": {"imports": ["KERNEL32.dll", "ODBC32.dll"]}}' ||
    fail "make_pe.py could not write a PE that imports ODBC32.dll"
  # The case reads RS_IMPORTS_DLL, requires the file to be readable before it runs dumpbin, and
  # requires dumpbin itself with RS_REQUIRE_DUMPBIN=1, so a missing tool fails rather than skips.
  out="$(RS_IMPORTS_DLL="$(windows_path "$planted")" RS_REQUIRE_DUMPBIN=1 \
    windows_program "$suite" -w NoTests "$case_name" 2>&1 | tr -d '\r')" || status=$?
  ((status != 0)) ||
    fail "the import-table case passed on a PE that imports ODBC32.dll, so it would not catch one"
  # Only the import check may fail, and Catch prints its expression with the failure. The planted
  # file's name holds no odbc32, so neither the case's INFO line nor dumpbin's "Dump of file" line
  # can stand in for it, and any other failure (an unreadable file, a dumpbin that refused the
  # file, a missing dumpbin) fails this step rather than passing it.
  grep -qF 'output.find("odbc32.dll")' <<<"$out" ||
    fail "the case failed, but not on the ODBC32.dll import: $(tr '\n' ' ' <<<"$out")"
  echo "windows-runner-tools: imports-negative: the import-table case fails a PE that imports" \
    "ODBC32.dll, on that import (exit $status)"
}

bare_names() {
  read_lock "$1"
  local system_root fake plant="$work/plant"
  system_root="${SYSTEMROOT:-${SystemRoot:-}}"
  [[ -n "$system_root" ]] || fail "SystemRoot is not set, so System32's version.dll cannot be found"
  # A file Windows maps, as it does not map a text file: a bare-name load that reached a planted
  # copy of it would load it, which is what the control shows.
  fake="$(posix_path "$system_root")/System32/version.dll"
  [[ -f "$fake" ]] || fail "no $fake to plant under the original names"
  printf '%s\n' "$lock" >"$work/lock"
  mkdir "$plant"
  python3 -I "$here/windows_bare_names.py" check "$work/lock" "$plant" "$fake" ||
    fail "a rewritten bare-name string mapped something, or could name a file (above)"
}

case "$step:$#" in
downloads:1) downloads ;;
admin-image:2) admin_image "$2" ;;
authenticode:2) authenticode "$2" ;;
written-bytes:2) written_bytes "$2" ;;
exports:2) extension_exports "$2" ;;
imports-negative:2) imports_negative "$2" ;;
bare-names:2) bare_names "$2" ;;
*) usage ;;
esac
