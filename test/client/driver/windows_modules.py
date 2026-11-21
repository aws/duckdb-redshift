"""Lists the modules a Windows process has loaded, to show that a windows_amd64 build without the
embedded driver loads no driver: a shell LOADs the extension and attempts a connect, which is
refused, and then its module list names the extension but no driver file, and the run created or
changed no driver or trust directory.

    python3 -I windows_modules.py self-test
    python3 -I windows_modules.py check <extension> <sql-file> <shell> [<shell argument>...]
    python3 -I windows_modules.py judge <extension> <module-list> <cache-directory> <shell-output>
    python3 -I windows_modules.py modules <module-list> <shell-output> <sql-file> <shell> [<arg>...]
    python3 -I windows_modules.py folders

self-test, on Windows: this process's module list names kernel32.dll, then a system DLL it had not
loaded once it loads one, and an enumeration given an invalid handle fails, naming the call.
check, on Windows: runs the shell, writes <sql-file> (the LOAD, an ATTACH with options, which opens
the connect that is refused, and a redshift_query on it, which finds no handle) to its
standard input and then a statement that creates a marker file, waits at most 5 minutes for the
marker, lists the shell's modules while it still runs, then closes its input, waits at most 1 minute
for it to exit, and judges the list and the shell's output. A shell that has to be killed, or exits
with any status but 1, the one DuckDB's shell gives after a statement failed, here the ATTACH and
the redshift_query, fails the check. The per-user cache is <FOLDERID_LocalAppData>\\duckdb-redshift,
found with SHGetKnownFolderPath as the loader finds it. <extension> is the path the shell loads.
check refuses to run while REDSHIFT_DRIVER_DIR is set, since the loader would then make its
directory elsewhere. The shell's output is kept in a temporary directory, which a failure names.
judge applies check's rules to a list of module paths, one per line, and to the shell's output, so
they run anywhere. Each fails with its name: control (kernel32.dll is listed, so the list is a
process's, checked first), extension (<extension> is listed, compared as Windows compares paths),
connect (the output holds the NOT_BUILT refusal naming windows_amd64, which a connect gets once it
has read the trust store and reached the driver source; an earlier refusal alone fails), driver (no
module is one of the driver set's or driver manager's files, and none lies in a .redshift-driver
directory) and created (no .redshift-driver or trust directory, or link of either name, no entry
inside one, and no *.pem file, in <extension>'s directory or the per-user cache, at any depth, and
no directory there it cannot inspect). The host may be shared with tests that make them, so check
records each one's size and last-write time before the shell starts and fails only on what the run
created or changed, and on any of them it removed (removed); judge counts everything it finds as
created.
modules, on Windows: runs the shell as check does and writes the module list, one path per line,
and the shell's output, judging neither; the end-to-end run (run-e2e.sh) judges them. folders, on
Windows: prints the long form of GetTempPathW() and the per-user cache, one per line.

On Linux the Win32 code runs only against stand-ins for its calls: OpenProcess, EnumProcessModules
and GetModuleFileNameExW failing, module lists that grow once, at each read or without end, a known
folder that fails, with or without a path, is null or is not absolute, and a temp folder made long,
or whose GetTempPathW or GetLongPathNameW fails. Only a Windows host makes the calls. There
self-test runs self_test and list_modules, check runs list_modules, per_user_cache and Windows, and
folders runs temp_folder and per_user_cache.

On failure it prints one line, "<rule>: <reason>", to stderr and exits 1. Standard library only.
"""

import ntpath
import os
import shutil
import subprocess
import sys
import tempfile
import time

DRIVER_ROOT = ".redshift-driver"
# The directories a connect could write a driver set or an exported trust store into, compared in
# lower case, as Windows compares names.
WATCHED_DIRECTORIES = (DRIVER_ROOT, "trust")
# The Windows driver set's files and the driver manager's, none of which a build without the
# embedded driver loads.
DRIVER_FILES = (
    "rsodbc.dll",
    "crypto.dll",
    "ssl.dll",
    "krb5_64.dll",
    "gssapi64.dll",
    "comerr64.dll",
    "k5sprt64.dll",
    "wshelp64.dll",
    "msvcr100.dll",
    "odbc32.dll",
    "odbccp32.dll",
)
# System DLLs a Python process does not load by itself; self-test loads the first one absent.
SELF_TEST_DLLS = ("dbghelp.dll", "winmm.dll", "wtsapi32.dll", "dwmapi.dll", "version.dll")
MARKER_SECONDS, EXIT_SECONDS = 300, 60
# DuckDB's shell exits 1 when a statement it read from standard input failed, as the refused connect
# does, and 0 when none did; anything else is a crash or a kill.
STATUS_AFTER_FAILED_STATEMENT = 1
# The connect refusal of a build without the embedded driver: the placeholder's NOT_BUILT message.
# Windows has a trust resolver, so a connect with SSL_CERT_FILE unset reads the store and reaches
# the driver source.
CONNECT_REFUSAL = "carries no Redshift ODBC driver for windows_amd64 "
# FOLDERID_LocalAppData, {F1B32785-6FBA-4FCF-9D55-7B8E7F157091}.
LOCAL_APP_DATA = (0xF1B32785, 0x6FBA, 0x4FCF, (0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91))


class Failure(Exception):
    pass


def comparable(path):
    """`path` as Windows compares it: without a \\\\?\\ prefix, normalised, case-insensitive."""
    if path.replace("/", "\\").startswith("\\\\?\\"):
        path = path[4:]
    return ntpath.normcase(ntpath.normpath(path))


def basenames(modules):
    return [ntpath.basename(comparable(m)) for m in modules]


def watched(extension, cache):
    """Every watched directory, entry inside one and *.pem file in `extension`'s directory and in
    `cache`, at any depth, without following links, mapped to its size and last-write time."""

    def refuse(error):
        # A missing base or a vanished entry holds nothing; a directory it cannot read fails closed.
        if not isinstance(error, FileNotFoundError):
            raise Failure("created: cannot inspect %s: %s" % (error.filename, error.strerror))

    def stamp(path):
        # A file rewritten in place keeps its path, so its size or last-write time shows the change.
        try:
            status = os.lstat(path)
        except FileNotFoundError:
            return None
        except OSError as e:
            raise Failure("created: cannot inspect %s: %s" % (path, e.strerror))
        return status.st_size, status.st_mtime_ns

    found = {}
    # ntpath: the extension's path is a Windows path, which judge also reads off Windows; a bare
    # name lies in the current directory.
    for base in (ntpath.dirname(extension) or os.curdir, cache):
        for root, directories, files in os.walk(base, onerror=refuse):
            relative = os.path.relpath(root, base).lower().replace("\\", "/").split("/")
            inside = any(part in WATCHED_DIRECTORIES for part in relative)
            for name in directories + files:
                if inside or name.lower() in WATCHED_DIRECTORIES or name.lower().endswith(".pem"):
                    path = os.path.join(root, name)
                    found[path] = stamp(path)
    return {path: entry for path, entry in found.items() if entry is not None}


def judge(extension, modules, cache, output, before=None):
    paths = [comparable(m) for m in modules]
    if "kernel32.dll" not in [ntpath.basename(p) for p in paths]:
        raise Failure("control: the module list names no kernel32.dll, so it is not a process's list")
    if comparable(extension) not in paths:
        raise Failure("extension: the module list does not name %s" % extension)
    if CONNECT_REFUSAL not in output:
        raise Failure(
            "connect: the shell's output holds no NOT_BUILT refusal for windows_amd64, so no "
            "connect reached the driver source"
        )
    for module, path in zip(modules, paths):
        parts = path.split("\\")
        if parts[-1] in DRIVER_FILES or DRIVER_ROOT in parts[:-1]:
            raise Failure("driver: the process loaded %s" % module)
    before, after = before or {}, watched(extension, cache)
    created = sorted(path for path, entry in after.items() if before.get(path) != entry)
    if created:
        raise Failure("created: the run created or changed %s" % ", ".join(created))
    removed = sorted(set(before) - set(after))
    if removed:
        raise Failure("removed: the run removed %s" % ", ".join(removed))
    print(
        "the shell attempted a connect (the NOT_BUILT refusal), loaded the extension and no driver "
        "module, and created or changed no driver or trust directory and no .pem file"
    )


def win32():
    """kernel32 with the signatures the enumeration uses; Windows only."""
    if os.name != "nt":
        raise Failure("windows: this command runs on Windows only")
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.K32EnumProcessModules.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.HMODULE),
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.K32EnumProcessModules.restype = wintypes.BOOL
    kernel32.K32GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    kernel32.K32GetModuleFileNameExW.restype = wintypes.DWORD
    return ctypes, wintypes, kernel32


def list_modules(handle, count=256):
    """The full path of each module the process `handle` names has loaded, read first into room for
    `count` modules."""
    ctypes, wintypes, kernel32 = win32()
    width = ctypes.sizeof(wintypes.HMODULE)
    for _ in range(8):  # a list that grows between calls is read again, a bounded number of times
        modules = (wintypes.HMODULE * count)()
        needed = wintypes.DWORD()
        if not kernel32.K32EnumProcessModules(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed)):
            raise Failure("enumerate: EnumProcessModules failed (Windows error %d)" % ctypes.get_last_error())
        if needed.value <= ctypes.sizeof(modules):
            break
        count = needed.value // width + 64
    else:
        raise Failure("enumerate: EnumProcessModules kept reporting a longer list")
    paths = []
    for module in modules[: needed.value // width]:
        path = ctypes.create_unicode_buffer(32768)
        if not kernel32.K32GetModuleFileNameExW(handle, module, path, len(path)):
            raise Failure("enumerate: GetModuleFileNameExW failed (Windows error %d)" % ctypes.get_last_error())
        paths.append(path.value)
    return paths


def self_test():
    ctypes, _, kernel32 = win32()
    before = basenames(list_modules(kernel32.GetCurrentProcess()))
    if "kernel32.dll" not in before:
        raise Failure("control: this process's module list names no kernel32.dll")
    if basenames(list_modules(kernel32.GetCurrentProcess(), 1)) != before:
        raise Failure("self-test: a list read again after a first call with room for one module differs")
    absent = [name for name in SELF_TEST_DLLS if name not in before]
    if not absent:
        raise Failure("self-test: every candidate DLL is already loaded, so none can show a new load")
    ctypes.WinDLL(absent[0])
    if absent[0] not in basenames(list_modules(kernel32.GetCurrentProcess())):
        raise Failure("self-test: the module list does not name %s after it was loaded" % absent[0])
    try:
        list_modules(None)
    except Failure as e:
        if "EnumProcessModules" not in str(e):
            raise Failure("self-test: an invalid handle failed without naming the call: %s" % e)
    else:
        raise Failure("self-test: an invalid handle was enumerated")
    print("self-test: the module list names kernel32.dll, the same when read again, and %s once loaded" % absent[0])


def per_user_cache():
    """<FOLDERID_LocalAppData>\\duckdb-redshift, as the loader finds it, not from %LOCALAPPDATA%."""
    ctypes, wintypes, _ = win32()

    class GUID(ctypes.Structure):
        _fields_ = [("a", wintypes.DWORD), ("b", wintypes.WORD), ("c", wintypes.WORD), ("d", ctypes.c_ubyte * 8)]

    folder = GUID(LOCAL_APP_DATA[0], LOCAL_APP_DATA[1], LOCAL_APP_DATA[2], (ctypes.c_ubyte * 8)(*LOCAL_APP_DATA[3]))
    shell32, ole32 = ctypes.WinDLL("shell32"), ctypes.WinDLL("ole32")
    shell32.SHGetKnownFolderPath.argtypes = [
        ctypes.POINTER(GUID),
        wintypes.DWORD,
        wintypes.HANDLE,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    shell32.SHGetKnownFolderPath.restype = ctypes.c_long
    ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]
    path = ctypes.c_void_p()
    result = shell32.SHGetKnownFolderPath(ctypes.byref(folder), 0, None, ctypes.byref(path))
    try:
        text = ctypes.wstring_at(path.value) if result == 0 and path.value else ""
    finally:
        ole32.CoTaskMemFree(path)  # the call allocates even when it fails
    if not ntpath.isabs(text):
        raise Failure(
            "directory: SHGetKnownFolderPath(FOLDERID_LocalAppData) found no folder (0x%08x)" % (result & 0xFFFFFFFF)
        )
    return ntpath.join(text, "duckdb-redshift")


class Windows(object):
    """What check reads from Windows: the per-user cache, and a running process's modules."""

    def __init__(self):
        win32()
        self.cache = per_user_cache()

    def modules(self, pid):
        ctypes, _, kernel32 = win32()
        handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)  # query and read
        if not handle:
            raise Failure("enumerate: OpenProcess failed for the shell (Windows error %d)" % ctypes.get_last_error())
        try:
            return list_modules(handle)
        finally:
            kernel32.CloseHandle(handle)


def run_shell(shell, sql, work, host):
    """Runs the shell on `sql` and the marker statement, its output in `work`, and returns the
    modules `host` lists for it once the marker exists, while it still runs."""
    marker = os.path.join(work, "marker.csv").replace("\\", "/")
    with open(os.path.join(work, "output.txt"), "wb") as output:
        try:
            process = subprocess.Popen(shell, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT)
        except OSError as e:
            # Its own message: Python 3.7's str(e) and e.strerror both repeat the path.
            raise Failure("shell: cannot start %s: %s" % (shell[0], os.strerror(e.errno) if e.errno else e))
        killed = False
        try:
            process.stdin.write(sql + b"\nCOPY (SELECT 1) TO '" + marker.replace("'", "''").encode() + b"';\n")
            process.stdin.flush()
            deadline = time.monotonic() + MARKER_SECONDS
            while not os.path.exists(marker):
                if process.poll() is not None:
                    raise Failure("shell: it exited (%d) before the statements ran" % process.returncode)
                if time.monotonic() > deadline:
                    raise Failure("shell: the statements did not finish within %d seconds" % MARKER_SECONDS)
                time.sleep(0.2)
            modules = host.modules(process.pid)
        finally:
            try:
                process.stdin.close()
            except OSError:
                pass  # the shell has exited; its exit is waited for below
            try:
                process.wait(EXIT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(EXIT_SECONDS)
                killed = True
    if killed:
        raise Failure("shell: it did not exit within %d seconds of its input closing" % EXIT_SECONDS)
    if process.returncode != STATUS_AFTER_FAILED_STATEMENT:
        raise Failure(
            "shell: it exited with status 0x%x, not 1, the status after a failed statement"
            % (process.returncode & 0xFFFFFFFF)
        )
    return modules


def check(extension, sql_file, shell, host=None):
    """check's command; `host` stands in for Windows in a test."""
    if os.environ.get("REDSHIFT_DRIVER_DIR") is not None:
        raise Failure(
            "directory: REDSHIFT_DRIVER_DIR is set, so a driver directory could be made where this "
            "check does not look; unset it"
        )
    host = host or Windows()
    extension = os.path.abspath(extension)
    with open(sql_file, "rb") as f:
        sql = f.read()
    work = tempfile.mkdtemp()
    try:
        before = watched(extension, host.cache)
        modules = run_shell(shell, sql, work, host)
        with open(os.path.join(work, "output.txt"), encoding="utf-8", errors="replace") as f:
            judge(extension, modules, host.cache, f.read(), before)
    except Failure as e:
        raise Failure("%s (the shell's output is in %s)" % (e, work))
    except (OSError, subprocess.TimeoutExpired) as e:
        raise Failure("shell: %s (the shell's output is in %s)" % (e, work))
    shutil.rmtree(work, ignore_errors=True)


def shell_modules(module_list, shell_output, sql_file, shell, host=None):
    """modules' command; `host` stands in for Windows in a test."""
    host = host or Windows()
    with open(sql_file, "rb") as f:
        sql = f.read()
    work = tempfile.mkdtemp()
    try:
        ran = False
        try:
            listed = run_shell(shell, sql, work, host)
            ran = True
        finally:
            # The caller's message on a failed run quotes the output too; a copy that fails then
            # must not hide the run's own failure.
            try:
                shutil.copyfile(os.path.join(work, "output.txt"), shell_output)
            except OSError:
                if ran:
                    raise
        with open(module_list, "w", encoding="utf-8", newline="\n") as f:
            f.write("".join("%s\n" % m for m in listed))
    except Failure as e:
        raise Failure("%s (the shell's output is in %s)" % (e, work))
    except (OSError, subprocess.TimeoutExpired) as e:
        raise Failure("shell: %s (the shell's output is in %s)" % (e, work))
    shutil.rmtree(work, ignore_errors=True)


def temp_folder():
    """The long form of GetTempPathW(), without its final separator, as the loader tests take it."""
    ctypes, wintypes, kernel32 = win32()
    kernel32.GetTempPathW.argtypes = [wintypes.DWORD, wintypes.LPWSTR]
    kernel32.GetTempPathW.restype = wintypes.DWORD
    kernel32.GetLongPathNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    kernel32.GetLongPathNameW.restype = wintypes.DWORD
    temp = ctypes.create_unicode_buffer(32768)
    long_form = ctypes.create_unicode_buffer(32768)
    if not kernel32.GetTempPathW(len(temp), temp):
        raise Failure("directory: GetTempPathW failed (Windows error %d)" % ctypes.get_last_error())
    length = kernel32.GetLongPathNameW(temp.value, long_form, len(long_form))
    if not length or length >= len(long_form):
        error = ctypes.get_last_error()
        raise Failure("directory: GetLongPathNameW failed (Windows error %d)" % error)
    return long_form.value.rstrip("\\")


def main(argv):
    try:
        if argv[1:] == ["self-test"]:
            self_test()
        elif len(argv) >= 5 and argv[1] == "check":
            check(argv[2], argv[3], argv[4:])
        elif len(argv) >= 6 and argv[1] == "modules":
            shell_modules(argv[2], argv[3], argv[4], argv[5:])
        elif argv[1:] == ["folders"]:
            print("%s\n%s" % (temp_folder(), per_user_cache()))
        elif len(argv) == 6 and argv[1] == "judge":
            with open(argv[3], encoding="utf-8") as f:
                modules = [line.rstrip("\r\n") for line in f if line.strip()]
            with open(argv[5], encoding="utf-8", errors="replace") as f:
                judge(argv[2], modules, argv[4], f.read())
        else:
            sys.stderr.write(
                "usage: windows_modules.py self-test\n"
                "       windows_modules.py check <extension> <sql-file> <shell> [<argument>...]\n"
                "       windows_modules.py judge <extension> <module-list> <cache-directory> <shell-output>\n"
                "       windows_modules.py modules <module-list> <shell-output> <sql-file> <shell>"
                " [<argument>...]\n"
                "       windows_modules.py folders\n"
            )
            return 2
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    except OSError as e:
        sys.stderr.write("file: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
