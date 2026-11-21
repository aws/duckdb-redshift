#!/usr/bin/env bash
# The check that a windows_amd64 build without the embedded driver loads no driver. It runs on a
# copy of the build's loadable extension once per shell, the stock one, then the official one, from
# Git's bash with a 64-bit Python 3.11 as python3:
#
#   bash test/client/driver/windows-stage1-check.sh <copy dir> <shell>
#
# resource   <copy dir>/redshift.duckdb_extension holds no RCDATA resource, where a Windows
#            build would embed the driver set; a missing file fails. pe.py runs directly, as
#            pe_test.sh's bounded() needs POSIX process groups; on Linux, pe_test.sh with
#            RS_LOAD_ONLY_EXTENSION naming a copy of the same file runs the same check.
# self-test  windows_modules.py lists this process's modules.
# install    the shell INSTALLs the copy into a new, empty extension directory, so a candidate an
#            earlier run left in the account's directory cannot be the one that loads.
# check      the shell LOADs it from there and runs third_party/redshift_odbc/tools/connect.sql's
#            attach and query, whose connect is refused; windows_modules.py check judges its
#            modules, its output and what the run created or changed beside the installed
#            extension and in the per-user cache.
#
# Prints a line per step, and exits 1 at the first that fails, naming it, or 2 on bad usage.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if (($# != 2)); then
  echo "usage: windows-stage1-check.sh <copy dir> <shell>" >&2
  exit 2
fi
copy="$1"
shell="$2"
say() { echo "windows-stage1-check: $*"; }
fail() {
  echo "windows-stage1-check: $*" >&2
  exit 1
}
# Windows programs take Windows paths, which Git's bash gives only when asked, in their long form, as
# the module list names a loaded file; elsewhere, as in the tool tests, a path is passed as it is.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m -l "$1"; else printf '%s\n' "$1"; fi; }
# quoted <text>: <text> as the inside of an SQL string literal.
quoted() {
  local quote="'"
  printf '%s' "${1//$quote/$quote$quote}"
}

extension="$copy/redshift.duckdb_extension"
connect="$here/../../../third_party/redshift_odbc/tools/connect.sql"
[[ -f "$extension" ]] || fail "resource: $extension is missing; give the directory the bundle was copied to"
[[ -f "$shell" && -x "$shell" ]] || fail "shell: $shell is not an executable DuckDB shell"

out="$(python3 -I "$here/../../../third_party/redshift_odbc/tools/lib/pe.py" rcdata "$(native "$extension")" 2>&1)" ||
  fail "resource: pe.py could not read $extension: $out"
[[ -z "$out" ]] || fail "resource: $extension holds RCDATA resources: ${out//$'\n'/; }"
say "resource: $extension holds no RCDATA resource"

python3 -I "$here/windows_modules.py" self-test || fail "self-test: windows_modules.py could not list this process's modules"

work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
mkdir "$work/extensions"
extensions="$(quoted "$(native "$work/extensions")")"
"$shell" -unsigned -c "SET extension_directory = '$extensions'; INSTALL '$(quoted "$(native "$extension")")'" ||
  fail "install: $shell could not INSTALL $extension"
installed=("$work/extensions"/*/*/redshift.duckdb_extension)
[[ ${#installed[@]} == 1 && -f "${installed[0]}" ]] ||
  fail "install: INSTALL did not put one copy in the new extension directory"
say "install: $shell installed it as ${installed[0]}"

[[ -s "$connect" ]] || fail "check: no connect.sql, or an empty one, at $connect"
{
  printf "SET extension_directory = '%s';\nLOAD redshift;\n" "$extensions"
  cat "$connect"
} >"$work/stage1.sql"
python3 -I "$here/windows_modules.py" check "$(native "${installed[0]}")" "$(native "$work/stage1.sql")" \
  "$(native "$shell")" -unsigned || fail "check: the shell's run failed the rule named above"
say "passed for $shell"
