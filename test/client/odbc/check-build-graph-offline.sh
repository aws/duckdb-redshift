#!/usr/bin/env bash
# Fails when the graph of a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=none build has a step that
# fetches or embeds the driver, or a download step of a kind forbidden_step knows, even one
# make test_client never runs: only the embedded build may have such steps. With
# --expect-embedded it checks the other way, that an embedded build's graph has both a fetch and
# an embed step, so a marker that stops matching the driver loader's steps fails the driver loader's tests.
#
#   check-build-graph-offline.sh [--expect-embedded] <build-dir>
#
# It reads only the graph of the generator CMakeCache.txt records, because a generator switch
# leaves the other generator's files in place. In a Unix Makefiles build it reads only the
# build.make of the targets CMakeFiles/TargetDirectories.txt lists, because a reconfigure leaves
# the directories of the targets it dropped, as make test_driver then make test_client does.
set -euo pipefail

expected_driver=none
if [[ "${1:-}" == --expect-embedded ]]; then expected_driver=embedded; shift; fi
build_dir="${1:?usage: check-build-graph-offline.sh [--expect-embedded] <build-dir>}"
cache="$build_dir/CMakeCache.txt"
fail() { echo "check-build-graph-offline: $*" >&2; exit 1; }
# CMake on Windows ends its files' lines with CRLF, as the Makefile's check_built_client_%
# allows: the values read from CMakeCache.txt and TargetDirectories.txt drop the \r, and the graph
# patterns and the path-boundary test below already treat it as whitespace.
cache_value() { sed -n "s/^$1=//p" "$cache" 2>/dev/null | tr -d '\r'; }

[[ "$(cache_value REDSHIFT_CLIENT:STRING)" == odbc &&
  "$(cache_value REDSHIFT_DRIVER:STRING)" == "$expected_driver" ]] ||
  fail "$build_dir is not a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=$expected_driver build; pass" \
    "build/release after make test_client (make test_driver with --expect-embedded), or build/tsan" \
    "after make test_client_tsan"

files=()
generator="$(cache_value CMAKE_GENERATOR:INTERNAL)"
case "$generator" in
Ninja) [[ ! -f "$build_dir/build.ninja" ]] || files+=("$build_dir/build.ninja") ;;
"Unix Makefiles")
  [[ -f "$build_dir/CMakeFiles/TargetDirectories.txt" ]] ||
    fail "$build_dir has no CMakeFiles/TargetDirectories.txt; re-run make test_client" \
      "(make test_client_tsan for build/tsan) to regenerate it"
  while IFS= read -r target; do
    target="${target%$'\r'}"
    [[ ! -f "$target/build.make" ]] || files+=("$target/build.make")
  done < "$build_dir/CMakeFiles/TargetDirectories.txt" ;;
*) fail "$build_dir was generated with '${generator:-no recorded generator}'; generate it with" \
     "Ninja or Unix Makefiles" ;;
esac
(( ${#files[@]} > 0 )) ||
  fail "$build_dir records $generator but has no $generator graph file; re-run make test_client" \
    "(make test_client_tsan for build/tsan) to regenerate it"

# The fetch and embed markers are the driver loader's names: its fetch step runs fetch-driver.sh and its
# embed step assembles a .S file or, on Windows, compiles the resource script embedded_driver_pe.rc
# (named in full, since DuckDB compiles .rc files of its own there). A change to these two markers
# changes the fixture cases that plant them (check-build-graph-offline_test.sh) too, and
# --expect-embedded on the driver loader's embedded graph shows both still match. Any other
# download is found only when it runs curl or wget; runs git clone, fetch, pull, submodule or
# ls-remote, with or without options before them (git -C dir fetch); runs pip install, download or
# wheel (pip3, python3 -m pip, with options); names a URL on its command line; or is an
# ExternalProject download step. CMake writes `sh -c "git clone x"` as sh -c git\ clone\ x, so
# escaped spaces count as spaces. Known passes: "/usr/bin/git" clone, git -C "/path with space"
# fetch, git fetch-pack, git remote update, git lfs pull and pipx, and a step that reads its URL
# from a file, such as cmake -P on a script calling file(DOWNLOAD). A download CMake makes while
# configuring (FetchContent, file(DOWNLOAD), execute_process) is not in the graph either; make
# test_client's no-network check catches those, on a build directory no networked run configured
# first. -P cannot be a marker, because DuckDB's own graph runs cmake -P to install and to append
# extension metadata. None of these markers matches the pinned DuckDB's own graph.
fetch_marker='fetch-driver[.]sh'
embed_marker='([.][sS][.]o|embedded_driver_pe[.]rc[.]res)([[:space:]:]|$)'
command_start='(^|[[:space:];&|/"'"'"'(])'
options='([[:space:]]+-[^[:space:]]+([[:space:]]+[^-[:space:]][^[:space:]]*)?)*'
download_marker="${command_start}(curl|wget)([[:space:]]|\$)|[[:alpha:]][[:alnum:]+.-]*://"
download_marker+="|-download([[:space:]:]|\$)"
download_marker+="|${command_start}git${options}[[:space:]]+(clone|fetch|pull|submodule|ls-remote)([^[:alnum:]_-]|\$)"
download_marker+="|(${command_start}pip[0-9.]*|-mpip)${options}[[:space:]]+(install|download|wheel)([^[:alnum:]_-]|\$)"
forbidden_step="$fetch_marker|$embed_marker|$download_marker"
# The checkout's path is on many lines, and a Unix Makefiles graph's dependency scan names it
# between spaces, so a checkout named curl would match. It is cut before matching, but only
# where it ends at a path boundary: cut as a plain substring, a checkout at /work would turn
# https://workers.example.com into https:/ers.example.com and hide the URL. Both values go
# through the environment, which awk reads verbatim, unlike -v. A Ninja edge's compiler and
# linker variables are skipped, because a Unix Makefiles build keeps compile flags in flags.make
# and link flags in link.txt, neither of which is read: an include path ending in curl or a define
# holding a URL is not a step.
checkout="$(cache_value redshift_SOURCE_DIR:STATIC)"
steps_matching() {  # steps_matching <pattern>; prints each matching graph line as file:line: text
  checkout="$checkout" pattern="$1" awk '
  BEGIN { checkout = ENVIRON["checkout"]; pattern = ENVIRON["pattern"] }
  FILENAME ~ /build[.]ninja$/ &&
    /^[[:space:]]+(DEFINES|INCLUDES|FLAGS|LANGUAGE_COMPILE_FLAGS|LINK_FLAGS|LINK_LIBRARIES|LINK_PATH)[[:space:]]*=/ { next }
  {
    line = ""; rest = $0
    while (checkout != "" && (i = index(rest, checkout))) {
      after = substr(rest, i + length(checkout), 1)
      at_boundary = after == "" || after ~ /[\/[:space:]"]/
      line = line substr(rest, 1, at_boundary ? i - 1 : i)
      rest = substr(rest, i + (at_boundary ? length(checkout) : 1))
    }
    line = line rest
    gsub(/\\[[:space:]]/, " ", line)
  }
  line ~ pattern { print FILENAME ":" FNR ": " $0 }' "${files[@]}"
}

if [[ "$expected_driver" == embedded ]]; then
  [[ -n "$(steps_matching "$fetch_marker")" ]] ||
    fail "the embedded build graph in $build_dir has no step matching $fetch_marker; update" \
      "fetch_marker in this script and its fixture cases, reviewed by the ODBC client's owners"
  [[ -n "$(steps_matching "$embed_marker")" ]] ||
    fail "the embedded build graph in $build_dir has no step matching $embed_marker; update" \
      "embed_marker in this script and its fixture cases, reviewed by the ODBC client's owners"
  echo "check-build-graph-offline: ok, the embedded graph in $build_dir has a fetch and an embed step"
  exit 0
fi
violations="$(steps_matching "$forbidden_step")"
[[ -z "$violations" ]] || fail "the build graph in $build_dir has a fetch, embed or download step;" \
  "add it only when REDSHIFT_DRIVER=embedded:"$'\n'"$violations"$'\n'"If a step is DuckDB's own and" \
  "downloads nothing, a DuckDB pin move added it: narrow forbidden_step in this script instead, with" \
  "review by the driver loader's owners."
echo "check-build-graph-offline: ok, no fetch, embed or recognised download step in $build_dir"
