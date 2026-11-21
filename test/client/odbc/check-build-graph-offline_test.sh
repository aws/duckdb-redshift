#!/usr/bin/env bash
# Fixture tests for check-build-graph-offline.sh. Run with `make test_graph_check`; make
# test_client and make test_client_tsan run them first.
# Each case plants the build directory of a checkout and checks the exit status and message.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

n=0; failed=0
check() {  # check <name> <expected-exit> <expected-substring> <script argument>...
  local name="$1" want="$2" needle="$3" out status
  n=$((n+1))
  out="$(bash "$here/check-build-graph-offline.sh" "${@:4}" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == *"$needle"* ]]; then
    echo "ok $n - $name"
  else
    echo "not ok $n - $name (exit $status, wanted $want; output: $out)"; failed=$((failed+1))
  fi
}

target=extension/redshift/CMakeFiles/redshift_extension.dir
# plant <checkout> [<generator> [<client> <driver>]]; prints the build directory,
# <checkout>/build/release. It writes the graph files of <generator> alone, as a directory that
# generator produced holds, so a case can plant another generator's files as stale ones. A Ninja
# graph compiles a file of the checkout; a Unix Makefiles graph runs CMake's dependency scan,
# which names the checkout's own path between spaces.
plant() {
  local checkout="$tmp/$1" d="$tmp/$1/build/release" generator="${2:-Ninja}"
  mkdir -p "$d/CMakeFiles" "$d/$target"
  printf '%s\n' "REDSHIFT_CLIENT:STRING=${3:-odbc}" "REDSHIFT_DRIVER:STRING=${4:-none}" \
    "CMAKE_GENERATOR:INTERNAL=$generator" "redshift_SOURCE_DIR:STATIC=$checkout" > "$d/CMakeCache.txt"
  echo "$d/$target" > "$d/CMakeFiles/TargetDirectories.txt"
  if [[ "$generator" == Ninja ]]; then
    echo "build x.cpp.o: CXX_COMPILER $checkout/src/x.cpp" > "$d/build.ninja"
  else
    printf '\tcd %s && cmake -E cmake_depends "Unix Makefiles" %s/duckdb %s %s\n' \
      "$d" "$checkout" "$checkout" "$d" > "$d/$target/build.make"
  fi
  echo "$d"
}
# plant_step <checkout> <graph-file> <step>; as plant, with <step> added to <graph-file>, whose
# name picks the generator: build.ninja is Ninja's and build.make a Unix Makefiles target's.
plant_step() {
  local d generator=Ninja
  [[ "$2" == build.ninja ]] || generator="Unix Makefiles"
  d="$(plant "$1" "$generator")"; echo "$3" >> "$d/$2"; echo "$d"
}
ok="check-build-graph-offline: ok"
not_none="is not a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=none build"

check "passes a Ninja none build that compiles a file of the checkout" 0 "$ok" "$(plant clean)"
check "passes a checkout whose own directory is named curl" 0 "$ok" "$(plant in/curl "Unix Makefiles")"
d="$(plant_step links-curl build.ninja '  LINK_LIBRARIES = -Wl,-rpath,/opt/curl  libcurlwrap.a')"
check "passes a build that links libcurl but runs no curl" 0 "$ok" "$d"
check "passes a checkout named curl whose path ends a line" 0 "$ok" \
  "$(plant_step eol/curl build.ninja "  COMMAND = cd $tmp/eol/curl")"

fetch="  COMMAND = cd $tmp/fetch/build/release && $tmp/fetch/third_party/redshift_odbc/tools/fetch-driver.sh"
d="$(plant_step fetch build.ninja "$fetch")"
check "fails on the driver fetch, quoting its line and saying where it may go" 1 \
  "add it only when REDSHIFT_DRIVER=embedded:"$'\n'"$d/build.ninja:2: $fetch" "$d"
check "fails on the assembler object that the embed step makes" 1 "embedded_driver_elf.S.o" \
  "$(plant_step embed build.ninja 'build embedded_driver_elf.S.o: ASM_COMPILER /s/embedded_driver_elf.S')"
check "fails on a lower-case assembler object in a Unix Makefiles graph" 1 "embedded_driver_elf.s.o" \
  "$(plant_step embed-make "$target/build.make" 'embedded_driver_elf.s.o: /s/embedded_driver_elf.s')"
check "fails on the resource object that the Windows embed step makes" 1 "embedded_driver_pe.rc.res" \
  "$(plant_step embed-pe build.ninja 'build ext/r.dir/embedded_driver_pe.rc.res: RC_COMPILER__r /b/embedded_driver_pe.rc')"
check "passes the resource objects of DuckDB's own Windows build" 0 "$ok" \
  "$(plant_step duckdb-rc build.ninja 'build src/duckdb.dir/version.rc.res: RC_COMPILER__duckdb /s/duckdb/src/version.rc')"
check "fails on a step that runs curl" 1 "/usr/bin/curl -o" \
  "$(plant_step runs-curl "$target/build.make" '	cd /b && /usr/bin/curl -o /b/x -K /b/curl.conf')"
check "fails on a step that runs wget" 1 "wget -O" \
  "$(plant_step wget build.ninja '  COMMAND = cd /b && wget -O /b/x -i /b/urls')"
check "fails on a step that names a URL" 1 "git clone file:///b/x.git" \
  "$(plant_step url build.ninja '  COMMAND = cd /b && /usr/bin/git clone file:///b/x.git')"
check "fails on an ExternalProject download step" 1 "dep-download" \
  "$(plant_step external build.ninja 'build dep-prefix/src/dep-stamp/dep-download: CUSTOM_COMMAND')"

# Under a checkout at /work, https://workers.example.com holds /work mid-name: cut as a plain
# substring, it would become https:/ers.example.com and hide the step.
d="$(plant_step work build.ninja '  COMMAND = cd /work/b && git clone https://workers.example.com/x.git')"
# Each rewrite below goes through a temporary file, because BSD sed reads -i's argument as a
# backup suffix. `;` not `&&`, so a failed rewrite stops the suite rather than letting the case
# pass on the unmodified cache.
sed 's|^redshift_SOURCE_DIR:STATIC=.*|redshift_SOURCE_DIR:STATIC=/work|' "$d/CMakeCache.txt" \
  > "$tmp/cache"; mv "$tmp/cache" "$d/CMakeCache.txt"
check "fails on a URL that holds the checkout's path mid-name" 1 "https://workers.example.com" "$d"

# The graph files CMake leaves behind after a generator switch, and the target directories an
# earlier configuration listed, are not this build's and must not fail it.
d="$(plant stale "Unix Makefiles")"; stale="$d/extension/redshift/CMakeFiles/redshift_driver_tests.dir"
mkdir -p "$stale"
printf '\tcd /b && /s/third_party/redshift_odbc/tools/fetch-driver.sh\n' > "$stale/build.make"
check "ignores the build.make of a target the configuration no longer lists" 0 "$ok" "$d"

d="$(plant stale-ninja "Unix Makefiles")"
echo "  COMMAND = cd /b && /s/third_party/redshift_odbc/tools/fetch-driver.sh" > "$d/build.ninja"
check "ignores a stale build.ninja in a Unix Makefiles build" 0 "$ok" "$d"

d="$(plant stale-make)"
printf '\tcd /b && /s/third_party/redshift_odbc/tools/fetch-driver.sh\n' > "$d/$target/build.make"
check "ignores a stale build.make in a listed target of a Ninja build" 0 "$ok" "$d"

d="$(plant two-targets "Unix Makefiles")"
second="$d/extension/redshift/CMakeFiles/redshift_loadable_extension.dir"
mkdir -p "$second"; echo "$second" >> "$d/CMakeFiles/TargetDirectories.txt"
printf '\tcd %s && true\n\tcd /b && /usr/bin/curl -o /b/x\n' "$d" > "$second/build.make"
check "fails on a step in the build.make of any listed target, quoting its file and line" 1 \
  "$second/build.make:2: "$'\t'"cd /b && /usr/bin/curl -o /b/x" "$d"

# CMake lists its built-in targets, such as edit_cache and install, in TargetDirectories.txt
# but makes no directory for them, so most listed targets have no build.make.
d="$(plant built-in-target "Unix Makefiles")"
echo "$d/CMakeFiles/edit_cache.dir" >> "$d/CMakeFiles/TargetDirectories.txt"
check "passes a Unix Makefiles build that lists a target with no build.make" 0 "$ok" "$d"

check "refuses a build that is not REDSHIFT_DRIVER=none" 1 "$not_none" "$(plant embedded Ninja odbc embedded)"
check "refuses a build that is not REDSHIFT_CLIENT=odbc" 1 "$not_none" "$(plant mock Ninja mock none)"
# CMake on Windows ends its files' lines with CRLF. A checkout named curl fails unless its path,
# like the settings and the generator, is read without the \r, and a target list read with it
# finds no build.make.
d="$(plant crlf/curl "Unix Makefiles")"
for f in CMakeCache.txt CMakeFiles/TargetDirectories.txt; do
  awk '{ printf "%s\r\n", $0 }' "$d/$f" > "$tmp/cache"; mv "$tmp/cache" "$d/$f"
done
check "reads a cache and target list whose lines end with CRLF" 0 "$ok" "$d"
d="$(plant no-graph)"; rm "$d/build.ninja"
check "fails when the recorded generator's graph file is missing" 1 "has no Ninja graph file" "$d"
d="$(plant no-target-list "Unix Makefiles")"; rm "$d/CMakeFiles/TargetDirectories.txt"
check "refuses a Unix Makefiles build with no target list" 1 "has no CMakeFiles/TargetDirectories.txt" "$d"
d="$(plant no-generator)"
grep -v '^CMAKE_GENERATOR:INTERNAL=' "$d/CMakeCache.txt" > "$tmp/cache"; mv "$tmp/cache" "$d/CMakeCache.txt"
check "refuses a build directory with no recorded generator" 1 "no recorded generator" "$d"

# Download forms that put no URL on the command line.
check "fails on an scp-style git clone" 1 "git clone git@" \
  "$(plant_step git-scp build.ninja '  COMMAND = cd /b && git clone git@github.com:org/repo.git')"
check "fails on a git submodule update" 1 "git submodule update" \
  "$(plant_step git-sub build.ninja '  COMMAND = cd /b && git submodule update --init third_party/x')"
check "fails on a pip install" 1 "pip install foo" \
  "$(plant_step pip build.ninja '  COMMAND = cd /b && python3 -m pip install foo')"
check "fails on a git fetch" 1 "git fetch" "$(plant_step git-fetch build.ninja '  COMMAND = git fetch')"
check "fails on a git pull" 1 "git pull" "$(plant_step git-pull build.ninja '  COMMAND = cd /b && git pull')"
check "fails on a git ls-remote" 1 "git ls-remote" \
  "$(plant_step git-ls build.ninja '  COMMAND = git ls-remote origin')"
check "fails on a git clone separated by a tab" 1 "clone" \
  "$(plant_step git-tab build.ninja "  COMMAND = git"$'\t'"clone x")"
check "fails on a git submodule after a -C option" 1 "submodule update" \
  "$(plant_step git-c build.ninja '  COMMAND = git -C /s/src submodule update --init')"
check "fails on a git clone CMake wrote with escaped spaces" 1 "git\\ clone" \
  "$(plant_step git-esc build.ninja '  COMMAND = cd /b && sh -c git\ clone\ git@github.com:org/repo.git')"
check "fails on a git fetch inside a quoted shell command" 1 "git fetch" \
  "$(plant_step git-quoted build.ninja "  COMMAND = bash -c 'git fetch origin'")"
check "fails on a pip3 install" 1 "pip3 install" "$(plant_step pip3 build.ninja '  COMMAND = pip3 install foo')"
check "fails on a pip download" 1 "pip download" "$(plant_step pip-dl build.ninja '  COMMAND = pip download x')"
check "fails on a pip install after an option" 1 "-q install" \
  "$(plant_step pip-q build.ninja '  COMMAND = python3 -m pip -q install foo')"
check "passes a step that names a file in a directory called pip" 0 "$ok" \
  "$(plant_step pip-dir build.ninja '  COMMAND = cmake -E copy /s/third_party/pip/install.txt /b/y')"

check "fails on a VERBATIM git pull with nothing after it" 1 'git pull"' \
  "$(plant_step git-verbatim build.ninja '  COMMAND = cd /b && sh -c "cd /s && git pull"')"
check "fails on a git fetch ended by a semicolon" 1 "git fetch;" \
  "$(plant_step git-semi "$target/build.make" '	bash -c "git fetch;git merge"')"
check "fails on a git pull in a subshell" 1 "(git pull)" \
  "$(plant_step git-sub-shell build.ninja '  COMMAND = sh -c "(git pull)"')"
check "fails on a pip wheel" 1 "pip wheel" "$(plant_step pip-wheel build.ninja '  COMMAND = pip wheel foo')"
check "fails on python3 -mpip" 1 "-mpip install" "$(plant_step mpip build.ninja '  COMMAND = python3 -mpip install foo')"
check "fails on a pip install after an option with a value" 1 "--cache-dir /c install" \
  "$(plant_step pip-val build.ninja '  COMMAND = python3 -m pip --cache-dir /c install foo')"
check "fails on a Ninja DESC naming a URL, as a Makefiles graph's echo line does" 1 "DESC = Fetching" \
  "$(plant_step desc build.ninja '  DESC = Fetching from https://example.com/x')"
check "fails on a pip download at the end of a VERBATIM command" 1 'pip download"' \
  "$(plant_step pip-verbatim build.ninja '  COMMAND = sh -c "cd /b && pip download"')"
check "passes a git fetch-pack, a known pass the comment lists" 0 "$ok" \
  "$(plant_step fetch-pack build.ninja '  COMMAND = git fetch-pack x')"
check "passes a step that echoes legit clone" 0 "$ok" \
  "$(plant_step legit build.ninja '  COMMAND = cmake -E echo legit clone')"
for v in DEFINES INCLUDES FLAGS LANGUAGE_COMPILE_FLAGS LINK_FLAGS LINK_LIBRARIES LINK_PATH; do
  check "passes a Ninja $v line ending in curl" 0 "$ok" "$(plant_step "var-$v" build.ninja "  $v = -x /opt/curl")"
done

# A Ninja edge's compiler and linker variables are not steps; a Unix Makefiles build keeps compile
# flags in flags.make and link flags in link.txt, neither of which is read.
check "passes a Ninja include path ending in curl" 0 "$ok" \
  "$(plant_step flags-curl build.ninja '  INCLUDES = -I/usr/include/curl')"
check "passes a Ninja link path ending in curl" 0 "$ok" \
  "$(plant_step link-curl build.ninja '  LINK_PATH = -L/opt/curl')"
check "passes a Ninja define holding a URL" 0 "$ok" \
  "$(plant_step defines-url build.ninja '  DEFINES = -DREDSHIFT_DOCS="see https://example.com/docs"')"

# --expect-embedded checks an embedded build's graph for both of the driver loader's markers.
embedded() {  # embedded <checkout> <fetch step or ""> <embed step or "">; prints the build directory
  local d; d="$(plant "$1" Ninja odbc embedded)"
  [[ -z "$2" ]] || echo "$2" >> "$d/build.ninja"
  [[ -z "$3" ]] || echo "$3" >> "$d/build.ninja"
  echo "$d"
}
fetch_step='  COMMAND = cd /b && /s/third_party/redshift_odbc/tools/fetch-driver.sh linux_amd64 /b/set'
embed_step='build ext/x.dir/embedded_driver_elf.S.o: ASM_COMPILER__x /s/embedded_driver_elf.S'
check "--expect-embedded passes an embedded graph with a fetch and an embed step" 0 \
  "has a fetch and an embed step" --expect-embedded "$(embedded both "$fetch_step" "$embed_step")"
check "--expect-embedded fails when no step matches the fetch marker" 1 "no step matching fetch-driver" \
  --expect-embedded "$(embedded no-fetch "" "$embed_step")"
check "--expect-embedded passes a Windows embedded graph, whose embed step compiles the resource script" 0 \
  "has a fetch and an embed step" --expect-embedded "$(embedded pe "$fetch_step" \
  'build ext/r.dir/embedded_driver_pe.rc.res: RC_COMPILER__r /b/embedded_driver_pe.rc')"
check "--expect-embedded fails when no step matches the embed marker" 1 "no step matching ([.][sS][.]o|embedded_driver_pe" \
  --expect-embedded "$(embedded no-embed "$fetch_step" "")"
check "--expect-embedded refuses a none build" 1 \
  "is not a REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=embedded build" --expect-embedded "$(plant expect-none)"
check "refuses an embedded build without --expect-embedded" 1 "$not_none" \
  "$(embedded plain "$fetch_step" "$embed_step")"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
