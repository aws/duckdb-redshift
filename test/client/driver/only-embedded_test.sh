#!/usr/bin/env bash
# Only the embedded driver can be loaded: nothing outside the loader's own directory
# (src/transport/driver) can reach its loader core or the platform loader, the shipped extension exposes
# no route to it, and the loader reads no setting other than the ones it documents. Source checks
# always run; the build checks run on an embedded build of build/release, which make test_driver makes
# first, and report SKIP without one, on a platform whose build has no loader (it links the NOT_BUILT
# placeholder: musl, FreeBSD), or on Windows, whose PE builds make test_driver's Windows branch checks.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="$root/build/release"
driver_src="$root/src/transport/driver"

n=0; failed=0
check() {  # check <name> <command...>
  local name="$1"; shift; n=$((n+1))
  if "$@"; then echo "ok $n - $name"; else echo "not ok $n - $name"; failed=$((failed+1)); fi
}

# Every file that includes the loader core, outside the loader's own source and test directories.
core_includes_outside() {
  grep -rlE '#include[[:space:]]*["<]([^">]*/)?driver_loader_core\.hpp[">]' "$root/src" "$root/test" 2>/dev/null |
    grep -vE "^($driver_src|$root/test/client/driver)/" || true
}
core_is_private() { [[ -z "$(core_includes_outside)" ]]; }

# The variables the loader's code reads: REDSHIFT_DRIVER_DIR (through its constant), XDG_CACHE_HOME and
# HOME, the presence check of the generated load-phase names, and whether ODBCINI is set before the first
# environment handle. No load-phase name's value is read, and no other
# way to the environment (the process's environ array, or the file /proc/self/environ) is used, so
# LD_LIBRARY_PATH's value is not read either.
getenv_lines() { grep -rhn 'getenv(' "$driver_src" | sed 's/^[0-9]*://'; }
reads_only_named_variables() {
  local line
  while IFS= read -r line; do
    case "$line" in
      *'getenv(DRIVER_DIR_VARIABLE)'* | *'getenv("XDG_CACHE_HOME")'* | *'getenv("HOME")'* | *'getenv(name.c_str())'*) ;;
      *'getenv(ODBC_INI_VARIABLE) != nullptr'*) ;;
      *) echo "unexpected: $line" >&2; return 1 ;;
    esac
  done < <(getenv_lines)
  # Calls, not lines: a second call on a line that passed the case above would read a value.
  # Compared as numbers: BSD wc pads its count with spaces.
  local calls lines name odbcini
  calls="$(getenv_lines | grep -o 'getenv(' | wc -l)"
  lines="$(getenv_lines | wc -l)"
  name="$(getenv_lines | grep -o 'getenv(name.c_str())' | wc -l)"
  odbcini="$(getenv_lines | grep -o 'getenv(ODBC_INI_VARIABLE)' | wc -l)"
  ((calls == lines && name == 1 && odbcini == 1))
}

# The one change the loader makes to the environment: ODBCINI set to /dev/null for each environment handle's allocation
# and unset after it, in driver_loader.cpp's AllocateWithoutOdbcIni, once each.
no_other_way_to_the_environment() {
  # Comments dropped, so this file's own words do not count.
  local file code hits="" sets=0 unsets=0
  while IFS= read -r file; do
    # The text is taken whole before it is searched: under pipefail, grep -q stopping at its first match can
    # make sed fail with SIGPIPE, and the file would then pass.
    code="$(sed 's://.*$::' "$file")"
    if [[ "$file" == "$driver_src/driver_loader.cpp" ]]; then
      sets="$(grep -cF '::setenv(ODBC_INI_VARIABLE, NO_ODBC_INI, 1)' <<<"$code" || true)"
      unsets="$(grep -cF '::unsetenv(ODBC_INI_VARIABLE);' <<<"$code" || true)"
      code="$(sed -e 's/::setenv(ODBC_INI_VARIABLE, NO_ODBC_INI, 1)//' -e 's/::unsetenv(ODBC_INI_VARIABLE);//' <<<"$code")"
    fi
    if grep -qE '/proc/(self|thread-self)/environ|\benviron\b|\bsetenv\b|\bputenv\b|\bunsetenv\b|\bclearenv\b' <<<"$code"; then
      hits="$hits $file"
    fi
  done < <(find "$driver_src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \))
  [[ -z "$hits" ]] || { echo "reaches the environment other than through the named getenv calls:$hits" >&2; return 1; }
  [[ "$sets" == 1 && "$unsets" == 1 ]] || { echo "ODBCINI is set $sets and unset $unsets times" >&2; return 1; }
}

# The loader registers nothing with DuckDB: it includes no DuckDB header at all.
registers_nothing() { [[ -d "$driver_src" ]] && ! grep -rqE '#include[[:space:]]*["<]duckdb' "$driver_src"; }

# Nothing outside the loader's directory calls the platform loader, and in it only the loader's own files do.
platform_loader_calls() {  # files whose code, comments dropped, calls dlopen
  local file code
  while IFS= read -r file; do
    code="$(sed 's://.*$::' "$file")" # whole, before it is searched: grep -q stopping early can make sed fail under pipefail
    if grep -qE '\bdlopen[[:space:]]*\(' <<<"$code"; then echo "$file"; fi
  done < <(find "$root/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \))
}
only_the_loader_calls_the_platform_loader() {
  local files unexpected
  files="$(platform_loader_calls)"
  [[ -n "$files" ]] || return 1
  unexpected="$(grep -vE "^$driver_src/(driver_loader|driver_loader_linux|driver_loader_macos)\.cpp\$" <<<"$files" || true)"
  [[ -z "$unexpected" ]] || { echo "calls dlopen: $unexpected" >&2; return 1; }
}

check "no file outside the loader's directories includes driver_loader_core.hpp" core_is_private
check "the loader reads only REDSHIFT_DRIVER_DIR, XDG_CACHE_HOME, HOME, the load-phase names and whether ODBCINI is set" reads_only_named_variables
check "the loader reaches the environment no other way: no environ, no /proc/self/environ, no setenv but ODBCINI's" no_other_way_to_the_environment
check "the loader includes no DuckDB header, so it registers no function or setting" registers_nothing
check "only the loader's own files call dlopen" only_the_loader_calls_the_platform_loader

cached() { sed -n "s/^$2=//p" "$1/CMakeCache.txt" 2>/dev/null | tr -d '\r'; }
# Why the shipped-build checks do not apply to the build directory $1 (empty when they do): they need an
# ODBC build with the embedded driver, on a platform that links the loader, in ELF or Mach-O (ar x, .cpp.o
# members, nm -D and Itanium names). Every ODBC configure defaults to REDSHIFT_DRIVER=embedded, including
# those of the platforms that link the placeholder.
build_checks_skipped() {
  local platform
  if [[ "$(cached "$1" REDSHIFT_CLIENT:STRING)" != odbc || "$(cached "$1" REDSHIFT_DRIVER:STRING)" != embedded ]]; then
    echo "build/release is not a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=embedded build"
    return
  fi
  platform="$(sed -n 's/^platform //p' "$1/extension/redshift/redshift_driver_identity.txt" 2>/dev/null | tr -d '\r')"
  case "$platform" in
    linux_amd64 | linux_arm64 | osx_amd64 | osx_arm64) ;;
    windows_amd64) echo "build/release is a windows_amd64 build, a PE one, which make test_driver's Windows branch checks" ;;
    *) echo "build/release is a ${platform:-platform-less} build, which links the NOT_BUILT placeholder and no loader" ;;
  esac
}
# The rule above, on build directories made here.
fixture_skips() {
  local tmp client driver platform result i=0
  tmp="$(mktemp -d)" || return 1
  trap 'rm -rf -- "${tmp:?}"' RETURN
  for spec in "odbc embedded linux_amd64:apply" "odbc embedded linux_arm64:apply" "odbc embedded osx_arm64:apply" \
    "odbc embedded windows_amd64:skip" "odbc embedded linux_amd64_musl:skip" "odbc none linux_amd64:skip" \
    "mock embedded linux_amd64:skip" "odbc embedded :skip"; do
    set -- $spec
    client="$1"; driver="$2"; platform="${3%%:*}"; result="${3##*:}"
    mkdir -p "$tmp/$i/extension/redshift"
    printf 'REDSHIFT_CLIENT:STRING=%s\nREDSHIFT_DRIVER:STRING=%s\n' "$client" "$driver" > "$tmp/$i/CMakeCache.txt"
    [[ -z "$platform" ]] || printf 'platform %s\n' "$platform" > "$tmp/$i/extension/redshift/redshift_driver_identity.txt"
    if [[ "$result" == apply ]]; then
      [[ -z "$(build_checks_skipped "$tmp/$i")" ]] || { echo "skipped but should apply: $spec" >&2; return 1; }
    else
      [[ -n "$(build_checks_skipped "$tmp/$i")" ]] || { echo "applies but should skip: $spec" >&2; return 1; }
    fi
    i=$((i + 1))
  done
}
fixture_skips_result=0; fixture_skips || fixture_skips_result=1
check "the shipped-build checks apply to an embedded Linux or macOS build and to no other" \
  test "$fixture_skips_result" = 0
# A Windows build's reason names the checks that do apply there, not the placeholder.
windows_reason() {
  local tmp result=0
  tmp="$(mktemp -d)" || return 1
  mkdir -p "$tmp/extension/redshift"
  printf 'REDSHIFT_CLIENT:STRING=odbc\nREDSHIFT_DRIVER:STRING=embedded\n' > "$tmp/CMakeCache.txt"
  printf 'platform windows_amd64\r\n' > "$tmp/extension/redshift/redshift_driver_identity.txt"
  [[ "$(build_checks_skipped "$tmp")" == *"make test_driver's Windows branch checks"* ]] || result=1
  rm -rf -- "${tmp:?}"
  return "$result"
}
check "a windows_amd64 build is skipped as a PE build that test_driver's Windows branch checks" windows_reason
skipped="$(build_checks_skipped "$build")"
if [[ -n "$skipped" ]]; then
  echo "ok $((n+1)) - shipped-build checks # SKIP $skipped"
else
  lib="$build/extension/redshift/libredshift_extension.a"
  loadable="$build/extension/redshift/redshift.duckdb_extension"
  # Apple's nm on macOS (a GNU nm earlier on PATH cannot read Mach-O), where a library's exported symbols
  # are its external defined ones; GNU nm's dynamic table on Linux. The long options are llvm-nm's, which
  # /usr/bin/nm is from Xcode 15 on. An object's functions are its T lines on ELF, where a weak (shared)
  # definition is W; Apple's nm prints a Mach-O weak definition (an inline or template instantiation) as T
  # too, so there they are read in its Mach-O form (-m): "(__TEXT,__text) external" or "private external",
  # with "weak" before it for a weak one. That form does not demangle (-C has no effect), so on macOS the
  # three functions are named as the object names them.
  if [[ "$(uname -s)" == Darwin ]]; then
    nm=/usr/bin/nm
    exports=(-g -U)
    defined=(-m --defined-only)
    function_line='^[0-9a-f]+ \(__TEXT,__text\) (private )?external '
    weak_line='\) weak '
    want_load='__ZN6duckdb15redshift_driver18LoadEmbeddedDriverEv'
    want_allocate='__ZN6duckdb15redshift_driver25AllocateEnvironmentHandleE'
    want_config='__ZN6duckdb15redshift_driver6detail14EmbeddedConfigEv'
  else
    nm=nm
    exports=(-D --defined-only)
    defined=(-C --defined-only)
    function_line=' T '
    weak_line=' [WV] '
    want_load='duckdb::redshift_driver::LoadEmbeddedDriver()'
    want_allocate='duckdb::redshift_driver::AllocateEnvironmentHandle('
    want_config='duckdb::redshift_driver::detail::EmbeddedConfig()'
  fi
  # The objects of the shipped extension, other than the loader's own, that refer to its core. The
  # loader's own are driver_loader.cpp.o, which defines it, and driver_embedded.cpp.o, which builds
  # the one DriverLoader of the process from the embedded data.
  core_users() {
    local tmp member symbols
    tmp="$(mktemp -d)" || return 1
    (cd "$tmp" && ar x "$lib")
    for member in "$tmp"/*.o; do
      case "$(basename "$member")" in driver_loader.cpp.o | driver_embedded.cpp.o) continue ;; esac
      symbols="$("$nm" -C --undefined-only "$member")" || { echo "nm failed on $(basename "$member")"; continue; }
      if grep -qE 'redshift_driver::detail::(LoadDriverFileSet|DriverLoader)' <<<"$symbols"; then
        basename "$member"
      fi
    done
    rm -rf -- "${tmp:?}"
  }
  only_the_loader_reaches_the_core() {
    local users
    # The status matters as much as the output: an empty answer from a core_users that failed early
    # would otherwise pass this check having examined no object.
    users="$(core_users)" || return 1
    [[ -z "$users" ]] || { echo "refers to the loader core: $users" >&2; return 1; }
  }
  # Of the objects that refer to the core, only driver_embedded.cpp.o makes a loader, and the functions
  # it exports are LoadEmbeddedDriver, AllocateEnvironmentHandle and the configuration it loads
  # (detail::EmbeddedConfig, which the tests give to a loader of their own, and which the loadable extension
  # does not export).
  only_load_embedded_driver_makes_a_loader() {
    local tmp symbols exported
    tmp="$(mktemp -d)" || return 1
    (cd "$tmp" && ar x "$lib") || { rm -rf -- "${tmp:?}"; return 1; }
    symbols="$("$nm" "${defined[@]}" "$tmp/driver_embedded.cpp.o")" || { rm -rf -- "${tmp:?}"; return 1; }
    rm -rf -- "${tmp:?}"
    exported="$(grep -E "$function_line" <<<"$symbols" || true)"
    [[ "$exported" == *"$want_load"* && "$exported" == *"$want_allocate"* && "$exported" == *"$want_config"* &&
      $(wc -l <<<"$exported") -eq 3 ]] || {
      echo "exports: $exported" >&2; return 1; }
    # No shared (weak) definition of its own names a loader either.
    ! grep -qE "$weak_line.*(DriverLoader|LoadDriverFileSet)" <<<"$symbols"
  }
  loader_is_in_the_shipped_build() {
    local members; members="$(ar t "$lib")" && grep -qx driver_loader.cpp.o <<<"$members" && grep -qx driver_embedded.cpp.o <<<"$members"
  }
  # The output is taken whole before it is searched: under pipefail, grep -q stopping early can make
  # nm fail with SIGPIPE, and a negated pipeline would then pass. A missing file fails.
  loadable_exports_no_loader_symbol() {
    local symbols
    [[ -f "$loadable" ]] && symbols="$("$nm" "${exports[@]}" -C "$loadable")" || return 1
    ! grep -qE 'redshift_driver|LoadDriverFileSet|DriverLoader|LoadEmbeddedDriver|AllocateEnvironmentHandle' <<<"$symbols"
  }
  check "the shipped extension carries the loader" loader_is_in_the_shipped_build
  check "only the loader's own objects refer to LoadDriverFileSet and DriverLoader" only_the_loader_reaches_the_core
  check "driver_embedded.cpp.o leads to a loader only through LoadEmbeddedDriver" only_load_embedded_driver_makes_a_loader
  check "the loadable extension exports no loader symbol" loadable_exports_no_loader_symbol
fi

echo "# $n cases, $((n - failed)) passed"
((failed == 0))
