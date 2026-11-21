#!/usr/bin/env bash
# The Windows loader's source scan: driver_loader_windows.cpp,
# driver_install_windows.cpp and driver_directory_windows.cpp call nothing that changes the
# process's DLL search or unloads a module, in any variant, and every LoadLibraryExW in the loader
# searches System32. A text scan of the source tree, with // comments left out, so it runs on every
# host and the driver suite, whose root run mounts no source, holds no such case.
# Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../../src/transport/driver"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
forbidden='SetDllDirectory|SetDefaultDllDirectories|AddDllDirectory|FreeLibrary'
forbidden+='|CreateActCtx|ActivateActCtx|LOAD_LIBRARY_SEARCH_APPLICATION_DIR'
# Each adds the folders a process's DLL search was given, or the application's, to the load.
forbidden+='|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_USER_DIRS'
# A plain LoadLibrary searches the default order, the application's folder first.
forbidden+='|LoadLibrary[AW]?\(|LoadLibraryExA'
# code <file>: <file> without its // comments, so a comment neither fails nor satisfies a rule. A
# comment starts a line or follows a space; a // inside a literal such as "file://x" does neither.
# A space then // inside a literal also cuts the line, hiding a call after it; none exists today.
code() { sed -E 's@(^|[[:space:]])//.*@@' "$1"; }
# calls <file>: the forbidden names <file> holds, on one line; empty when it holds none.
calls() { { code "$1" | grep -o -E "$forbidden" || true; } | tr -d '(' | LC_ALL=C sort -u | tr '\n' ' '; }
# unsearched <file>: why <file> has a LoadLibraryExW without System32's flag; empty when it has
# none. Counted by line, so each call names the flag on its own line or the next.
unsearched() {
  local loads flags
  loads="$(code "$1" | grep -c 'LoadLibraryExW(' || true)"
  flags="$(code "$1" | grep -c 'LOAD_LIBRARY_SEARCH_SYSTEM32' || true)"
  if (( flags < loads )); then echo "$loads LoadLibraryExW calls, $flags lines with System32's flag"; fi
}

for file in driver_loader_windows.cpp driver_install_windows.cpp driver_directory_windows.cpp; do
  if ! grep -q 'namespace windows_driver' "$src/$file"; then
    fail "$file is the Windows loader's (it holds no namespace windows_driver)"
  elif [[ -n "$(calls "$src/$file")" ]]; then
    fail "$file calls nothing that changes the DLL search or unloads ($(calls "$src/$file"))"
  else
    pass "$file calls nothing that changes the DLL search or unloads"
  fi
done
if [[ -z "$(unsearched "$src/driver_loader_windows.cpp")" ]]; then
  pass "every LoadLibraryExW in driver_loader_windows.cpp searches System32"
else fail "every LoadLibraryExW in driver_loader_windows.cpp searches System32 ($(unsearched \
  "$src/driver_loader_windows.cpp"))"; fi
# The scan itself: a planted use of every forbidden name is found, and so is a load without System32.
cp "$src/driver_loader_windows.cpp" "$tmp/planted.cpp"
cat >> "$tmp/planted.cpp" <<'PLANTED'
void Planted() {
	SetDllDirectoryW(L""); SetDefaultDllDirectories(0); AddDllDirectory(L""); FreeLibrary(nullptr);
	CreateActCtxW(nullptr); ActivateActCtx(nullptr, 0); LoadLibraryW(L""); LoadLibraryA(""); LoadLibrary("");
	LoadLibraryExA("", nullptr, 0); LoadLibraryExW(L"", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
	LoadLibraryExW(L"", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
}
PLANTED
expected="ActivateActCtx AddDllDirectory CreateActCtx FreeLibrary LOAD_LIBRARY_SEARCH_APPLICATION_DIR "
expected+="LOAD_LIBRARY_SEARCH_DEFAULT_DIRS LOAD_LIBRARY_SEARCH_USER_DIRS "
expected+="LoadLibrary LoadLibraryA LoadLibraryExA LoadLibraryW SetDefaultDllDirectories SetDllDirectory "
if [[ "$(calls "$tmp/planted.cpp")" == "$expected" ]]; then
  pass "the scan finds a planted use of every forbidden name"
else fail "the scan finds a planted use of every forbidden name: $(calls "$tmp/planted.cpp")"; fi
if [[ -n "$(unsearched "$tmp/planted.cpp")" ]]; then
  pass "the scan finds a planted LoadLibraryExW without System32"
else fail "the scan finds a planted LoadLibraryExW without System32"; fi
# A comment is not code: one naming System32's flag does not cover a load, nor a name fail the scan.
cp "$src/driver_loader_windows.cpp" "$tmp/commented.cpp"
printf '%s\n' 'void Commented() { LoadLibraryExW(L"", nullptr, 0); } // LOAD_LIBRARY_SEARCH_SYSTEM32' \
  '// FreeLibrary(module); SetDllDirectoryW(L"");' >> "$tmp/commented.cpp"
if [[ -n "$(unsearched "$tmp/commented.cpp")" && -z "$(calls "$tmp/commented.cpp")" ]]; then
  pass "a comment neither covers a LoadLibraryExW without System32 nor fails the scan"
else fail "a comment neither covers a LoadLibraryExW without System32 nor fails the scan"; fi
cp "$src/driver_loader_windows.cpp" "$tmp/literal.cpp"
printf '%s\n' 'void Literal(HMODULE m) { const wchar_t *u = L"file://x"; FreeLibrary(m); }' >> "$tmp/literal.cpp"
if [[ "$(calls "$tmp/literal.cpp")" == "FreeLibrary " ]]; then
  pass "a // inside a string literal does not hide the code after it"
else fail "a // inside a string literal does not hide the code after it: $(calls "$tmp/literal.cpp")"; fi
echo "# $n cases, $((n - failed)) passed"
exit $((failed > 0))
