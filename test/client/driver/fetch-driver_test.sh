#!/usr/bin/env bash
# Tool tests for third_party/redshift_odbc/tools/fetch-driver.sh. Run by run-tool-tests.sh.
#
# The cases that extract use the real Linux packages of the lock under test, kept between runs in
# build/driver-package-cache/<platform>/ at the repository root. The first case for each platform
# fetches its package there with fetch-driver.sh itself, which takes it from RS_DRIVER_MIRROR when
# the caller's holds it, else from the copy an earlier run kept, else over the lock's URL: that
# download, once per platform on a cold cache, is the suite's only network access, so the suite
# runs offline with RS_DRIVER_MIRROR set or a warm cache, and has the same cases either way. Every
# other case serves the linux_amd64 package through RS_DRIVER_MIRROR or through a stand-in curl
# that records its arguments, and a stand-in that fails when called proves a case made no download.
# The macOS cases use packages make_xar.py writes, locked in a copy of the lock, and the real
# universal package of the lock's osx_arm64 row, which release-cache.sh copies from
# RS_DRIVER_MIRROR or downloads once into the same cache.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
tools="$repo/third_party/redshift_odbc/tools"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# The header names each file by its real path, so the paths compared with it are real too: a
# TMPDIR under a symbolic link (macOS's /var/folders is under /private) must not change them.
tmp="$(cd "$tmp" && pwd -P)"

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
# The kept copy is always written as a new file and renamed in, so an unchanged inode shows that a
# run left it as it was rather than copying it again. python3, not stat, whose flags differ on macOS.
inode() { python3 -I -c 'import os,sys; print(os.stat(sys.argv[1]).st_ino)' "$1"; }
# same <a> <b>: both files exist with the same bytes; compared by SHA-256, as cmp may be missing.
same() { [[ -f "$1" && -f "$2" && "$(sha "$1")" == "$(sha "$2")" ]]; }

# The lock under test: RS_DRIVER_LOCK when the caller sets one (a lock other than the checked-in
# one), else the checked-in one; every case below derives from it, so the tests exercise that
# lock's driver and reuse its package.
base_lock="${RS_DRIVER_LOCK:-$repo/third_party/redshift_odbc/driver.lock}"
[[ "$base_lock" == /* ]] || base_lock="$PWD/$base_lock"
checked_lock="$(bash "$tools/check-lock.sh" "$base_lock")"
lock_row="$(printf '%s\n' "$checked_lock" | awk '$1 == "driver" && $2 == "linux_amd64"')"
read -r _ _ _ drv_sha _ url pkg_sha path_in_pkg <<<"$lock_row"
cache_root="$repo/build/driver-package-cache"
# make test_driver has just fetched the host platform's package into the embedded build's kept
# copies; reuse it rather than fetching a second time. The kept copy is named by
# its SHA-256, so each platform's row says which one it would be.
while read -r _ row_p _ _ _ _ row_pkg _; do
  build_copy="$repo/build/release/extension/redshift/redshift_driver/packages/$row_pkg"
  if [[ ! -f "$cache_root/$row_p/packages/$row_pkg" && -f "$build_copy" && "$(sha "$build_copy")" == "$row_pkg" ]]; then
    mkdir -p "$cache_root/$row_p/packages" && cp "$build_copy" "$cache_root/$row_p/packages/$row_pkg"
  fi
done < <(printf '%s\n' "$checked_lock" | awk '$1 == "driver"')
# first_fetch <platform> <ELF machine>: the case that fetches the platform's real package into its
# cache, with the caller's RS_DRIVER_MIRROR and the real curl, and accepts the locked driver; for
# linux_arm64 this is the case that holds the aarch64 machine number to the real package.
first_fetch() {
  local p="$1" row d_sha text status
  row="$(printf '%s\n' "$checked_lock" | awk -v p="$p" '$1 == "driver" && $2 == p')"
  if [[ -z "$row" ]]; then echo "ok $((n += 1)) - fetches the real $p package # SKIP the lock has no $p row"; return; fi
  read -r _ _ _ d_sha _ <<<"$row"
  text="$(env RS_DRIVER_LOCK="$base_lock" bash "$tools/fetch-driver.sh" "$p" "$cache_root/$p" 2>&1)" && status=0 || status=$?
  printf '%s\n' "$text" | sed -n 's/^fetch-driver: /# /p'
  if [[ "$status" == 0 && "$(sha "$cache_root/$p/librsodbc64.so")" == "$d_sha" ]] &&
    grep -qx "#define REDSHIFT_DRIVER_PLATFORM \"$p\"" "$cache_root/$p/driver_info.h"; then
    pass "fetches the real $p package and accepts its $2 driver"
  else fail "fetches the real $p package (exit $status; $text)"; fi
}
first_fetch linux_amd64 x86-64
first_fetch linux_arm64 aarch64
mirror="$cache_root/linux_amd64/packages"

# A stand-in curl: FAKE_CURL=fail exits 99 when called, with a line on stderr;
# FAKE_CURL=exit:<status>:<message> exits <status> with <message> on stderr, as curl does;
# FAKE_CURL=<code>:<file> writes <file> to --output and prints <code> for --write-out. Every call's
# arguments go to $tmp/curl.args, and its core-file limit to $tmp/curl.core-limit.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/curl" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$FAKE_CURL_ARGS"
ulimit -c > "$FAKE_CURL_CORE_LIMIT"
case "$FAKE_CURL" in
fail) echo "curl: (99) the stand-in fails" >&2; exit 99 ;;
exit:*) e="${FAKE_CURL#exit:}"; echo "${e#*:}" >&2; exit "${e%%:*}" ;;
esac
out="" format=""
while (($#)); do case "$1" in --output) out="$2" ;; --write-out) format="$2" ;; esac; shift; done
# Like curl, it prints the status only when asked to with --write-out %{http_code}.
[[ -n "$out" && "$format" == '%{http_code}' ]] || exit 98
# A write past the file-size limit (ulimit -f) kills cat, as it kills curl, with SIGXFSZ: status 153,
# which the stand-in passes on as curl's; any other write error is curl's 23. cat, not cp, which on
# macOS may copy without write(2) (an APFS clone), past the limit.
cat -- "${FAKE_CURL#*:}" > "$out" || { s=$?; ((s == 153)) && exit 153; exit 23; }
printf '%s' "${FAKE_CURL%%:*}"
EOF
chmod +x "$tmp/bin/curl"
export FAKE_CURL_ARGS="$tmp/curl.args" FAKE_CURL_CORE_LIMIT="$tmp/curl.core-limit"

# shellcheck source=bounded.sh
source "$here/bounded.sh" # bounded <command>...: at most 20 seconds, its process group stopped
# run <out> [env...] [-- <tools-dir>]: runs fetch-driver.sh for linux_amd64 with the stand-in curl
# first on PATH; sets out_text and status.
run() {
  local o="$1"; shift; local t="$tools" e=()
  while (($#)); do if [[ "$1" == -- ]]; then t="$2"; shift 2; else e+=("$1"); shift; fi; done
  rm -f "$FAKE_CURL_ARGS" "$FAKE_CURL_CORE_LIMIT"
  # A caller's RS_DRIVER_MIRROR would change what is under test; the lock is base_lock.
  out_text="$(env -u RS_DRIVER_MIRROR RS_DRIVER_LOCK="$base_lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail ${e[@]+"${e[@]}"} \
    bash "$t/fetch-driver.sh" linux_amd64 "$o" 2>&1)" && status=0 || status=$?
}
# After any run, passing or failing, its work directory is gone.
no_work() { ! compgen -G "$1/.fetch-*" >/dev/null; }
set_ok() {
  [[ -f "$1/driver_info.h" && "$(sha "$1/librsodbc64.so")" == "$drv_sha" && -f "$1/THIRD_PARTY_LICENSES" ]] && no_work "$1"
}
# No header, which marks a complete set, and none of the set's files, not even a dangling link.
no_set() {
  local f
  for f in driver_info.h librsodbc64.so LICENSE NOTICE THIRD_PARTY_LICENSES; do
    [[ ! -e "$1/$f" && ! -L "$1/$f" ]] || return 1
  done
  no_work "$1"
}
expect_fail() {  # expect_fail <name> <substring> <out>
  if [[ "$status" == 1 && "$out_text" == *"$2"* ]] && no_set "$3"; then pass "$1"; else
    fail "$1 (exit $status; output: $out_text)"; fi
}
expect_line() {  # expect_line <name> <line> <out>: as expect_fail, with <line> the whole output
  if [[ "$status" == 1 && "$out_text" == "$2" ]] && no_set "$3"; then pass "$1"; else
    fail "$1 (exit $status; output: $out_text)"; fi
}

o="$tmp/mirror"; run "$o" "RS_DRIVER_MIRROR=$mirror"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ ! -e "$FAKE_CURL_ARGS" && "$out_text" == *"package from RS_DRIVER_MIRROR"* ]]; then
  pass "uses RS_DRIVER_MIRROR when set and downloads nothing"; else fail "uses RS_DRIVER_MIRROR (exit $status; $out_text)"; fi
kept_inode="$(inode "$o/packages/$pkg_sha")"
run "$o"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ ! -e "$FAKE_CURL_ARGS" && "$out_text" == *"the copy kept in"* &&
  "$(inode "$o/packages/$pkg_sha")" == "$kept_inode" ]]; then
  pass "a second run reuses the package kept in <out-dir>/packages and does not write it again"
  else fail "reuses its kept copy (exit $status; $out_text)"; fi
if grep -q "REDSHIFT_DRIVER_SET_SHA256" "$o/driver_info.h" && [[ "$(sha "$o/LICENSE")" == "$(sha "$repo/third_party/redshift_odbc/LICENSE")" ]] &&
  grep -q "X(driver, \"$o/librsodbc64.so\")" "$o/driver_info.h"; then
  pass "leaves the notices and driver_info.h, naming the files where they are, beside the driver"; else fail "notices and header"; fi

mkdir -p "$tmp/bad-mirror"; printf 'not the package' > "$tmp/bad-mirror/$pkg_sha"
o="$tmp/tampered"; run "$o" "RS_DRIVER_MIRROR=$tmp/bad-mirror"
expect_fail "a tampered mirror file fails, naming the platform, the file and both hashes" \
  "linux_amd64: package $(basename "$url") from RS_DRIVER_MIRROR has SHA-256 $(sha "$tmp/bad-mirror/$pkg_sha"), the lock expects $pkg_sha" "$o"
o="$tmp/warm-cache"; run "$o" "RS_DRIVER_MIRROR=$mirror"; run "$o" "RS_DRIVER_MIRROR=$tmp/bad-mirror"
expect_fail "a tampered mirror file fails even when a good kept copy exists" "from RS_DRIVER_MIRROR has SHA-256" "$o"
o="$tmp/oversize"; mkdir -p "$tmp/big-mirror"; truncate -s 268435457 "$tmp/big-mirror/$pkg_sha"
run "$o" "RS_DRIVER_MIRROR=$tmp/big-mirror"
expect_fail "a package larger than the bound fails before it is read" "is larger than 268435456 bytes" "$o"
o="$tmp/empty-mirror"; mkdir -p "$tmp/empty"; run "$o" "RS_DRIVER_MIRROR=$tmp/empty" "FAKE_CURL=200:$mirror/$pkg_sha"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ "$out_text" == *"RS_DRIVER_MIRROR has no $pkg_sha; trying the kept copy, then $url"* &&
  "$(tail -1 "$FAKE_CURL_ARGS")" == "$url" ]]; then
  pass "a mirror without the package falls back to the lock's URL, saying so"; else fail "mirror fallback (exit $status; $out_text)"; fi
# The same mirror again: now the copy that run kept is used, and nothing is downloaded.
run "$o" "RS_DRIVER_MIRROR=$tmp/empty"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ ! -e "$FAKE_CURL_ARGS" && "$out_text" == *"RS_DRIVER_MIRROR has no $pkg_sha"* &&
  "$out_text" == *"package from the copy kept in"* ]]; then
  pass "a mirror without the package falls back to a good kept copy before the URL"; else fail "mirror miss, kept copy (exit $status; $out_text)"; fi

# With no mirror, so the kept copy is the first source looked at.
o="$tmp/damaged-copy"; mkdir -p "$o/packages"; printf 'damaged' > "$o/packages/$pkg_sha"
run "$o" "FAKE_CURL=200:$mirror/$pkg_sha"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ -e "$FAKE_CURL_ARGS" && "$(sha "$o/packages/$pkg_sha")" == "$pkg_sha" &&
  "$out_text" == *"the copy kept in $o/packages has SHA-256 $(printf damaged | sha /dev/stdin), the lock expects $pkg_sha; replacing it"* &&
  "$out_text" != *"package from the copy kept in"* ]]; then
  pass "a damaged kept copy is skipped and replaced"; else fail "damaged kept copy (exit $status; $out_text)"; fi
o="$tmp/oversize-copy"; mkdir -p "$o/packages"; truncate -s 268435457 "$o/packages/$pkg_sha"
run "$o" "FAKE_CURL=200:$mirror/$pkg_sha"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ -e "$FAKE_CURL_ARGS" && "$(sha "$o/packages/$pkg_sha")" == "$pkg_sha" &&
  "$out_text" == *"the copy kept in $o/packages is larger than 268435456 bytes; replacing it"* ]]; then
  pass "a kept copy larger than the bound is skipped unread and replaced"; else fail "oversized kept copy (exit $status; $out_text)"; fi
# What stands at <out-dir>/packages/<sha>, when it is looked at, or in the mirror must be a regular
# file: a FIFO is not opened (the test gives up after 20 seconds), a directory takes no copy, and a
# link is not followed.
o="$tmp/fifo-copy"; mkdir -p "$o/packages"; mkfifo "$o/packages/$pkg_sha"
out_text="$(bounded env -u RS_DRIVER_MIRROR RS_DRIVER_LOCK="$base_lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
  bash "$tools/fetch-driver.sh" linux_amd64 "$o" 2>&1)" && status=0 || status=$?
expect_fail "a FIFO kept as the package fails at once, naming it" "$o/packages/$pkg_sha is not a regular file (a link, FIFO, directory or device); remove it" "$o"
o="$tmp/dir-copy"; mkdir -p "$o/packages/$pkg_sha"
run "$o"
if [[ "$status" == 1 && "$out_text" == *"$o/packages/$pkg_sha is not a regular file"*"; remove it"* ]] && no_set "$o" &&
  [[ -z "$(ls -A "$o/packages/$pkg_sha")" ]]; then
  pass "a directory kept as the package fails, naming it, and takes no copy"; else fail "directory kept copy (exit $status; $out_text)"; fi
# With the mirror holding the package, the kept path is not looked at: nothing stale there stops the
# build, and none of it is read, opened or replaced.
for kind in directory fifo link damaged; do
  o="$tmp/mirror-over-$kind"; mkdir -p "$o/packages"
  case "$kind" in
  directory) mkdir "$o/packages/$pkg_sha" ;;
  fifo) mkfifo "$o/packages/$pkg_sha" ;;
  link) ln -s "$tmp/nowhere" "$o/packages/$pkg_sha" ;;
  damaged) printf 'damaged' > "$o/packages/$pkg_sha" ;;
  esac
  out_text="$(bounded env RS_DRIVER_MIRROR="$mirror" RS_DRIVER_LOCK="$base_lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
    bash "$tools/fetch-driver.sh" linux_amd64 "$o" 2>&1)" && status=0 || status=$?
  left=false
  case "$kind" in
  directory) [[ -d "$o/packages/$pkg_sha" && -z "$(ls -A "$o/packages/$pkg_sha")" ]] && left=true ;;
  fifo) [[ -p "$o/packages/$pkg_sha" ]] && left=true ;;
  link) [[ -L "$o/packages/$pkg_sha" && ! -e "$tmp/nowhere" ]] && left=true ;;
  damaged) [[ "$(cat "$o/packages/$pkg_sha")" == damaged ]] && left=true ;;
  esac
  if [[ "$status" == 0 ]] && set_ok "$o" && $left && [[ "$out_text" == *"package from RS_DRIVER_MIRROR"* &&
    "$out_text" != *"$o/packages"* ]]; then
    pass "with the mirror holding the package, a $kind at the kept path neither stops the build nor is touched"
  else fail "mirror over a kept $kind (exit $status; $out_text)"; fi
done
mkdir -p "$tmp/link-mirror"; ln -s "$mirror/$pkg_sha" "$tmp/link-mirror/$pkg_sha"
o="$tmp/link-mirror-out"; run "$o" "RS_DRIVER_MIRROR=$tmp/link-mirror"
expect_fail "a link in the mirror is not followed" "$tmp/link-mirror/$pkg_sha is not a regular file" "$o"
# A dangling link is something there too: refused, not taken for a missing package.
mkdir -p "$tmp/dangling-mirror"; ln -s "$tmp/nowhere" "$tmp/dangling-mirror/$pkg_sha"
o="$tmp/dangling-mirror-out"; run "$o" "RS_DRIVER_MIRROR=$tmp/dangling-mirror"
expect_fail "a dangling link in the mirror is refused, not taken for a missing package" \
  "$tmp/dangling-mirror/$pkg_sha is not a regular file" "$o"

(cd "$tmp" && run relative "RS_DRIVER_MIRROR=$mirror" && [[ "$status" == 0 ]] && set_ok "$tmp/relative") &&
  pass "a relative out-dir works" || fail "a relative out-dir works"

o="$tmp/download"; run "$o" "FAKE_CURL=200:$mirror/$pkg_sha"
# has_pair <flag> <value>: the stand-in curl was given <flag> immediately followed by <value>.
has_pair() { awk -v f="$1" -v v="$2" 'prev == f && $0 == v {ok=1} {prev=$0} END {exit !ok}' "$FAKE_CURL_ARGS"; }
if [[ "$status" == 0 ]] && set_ok "$o" && [[ "$(head -1 "$FAKE_CURL_ARGS")" == -q && "$(tail -1 "$FAKE_CURL_ARGS")" == "$url" ]] &&
  has_pair --proto '=https' && has_pair --proto-redir '=https' && has_pair --max-redirs 0 && has_pair --speed-time 60 &&
  has_pair --max-time 600 && has_pair --max-filesize 268435456 && has_pair --speed-limit 1024 &&
  has_pair --retry 3 && has_pair --retry-max-time 600 && has_pair --connect-timeout 30 &&
  grep -qx -- --path-as-is "$FAKE_CURL_ARGS" && grep -qx -- --fail "$FAKE_CURL_ARGS" &&
  ! grep -qE -- '^(-L|--location|-k|--insecure)$' "$FAKE_CURL_ARGS"; then
  pass "downloads the lock's URL as written: .curlrc ignored, HTTPS only, no redirects, path as is, bounded by curl's own limits and --max-filesize"
else fail "download arguments (exit $status; $out_text; args: $(tr '\n' ' ' < "$FAKE_CURL_ARGS" 2>/dev/null))"; fi
# curl alone: download.sh runs with no timeout on PATH, as on a fresh macOS host without coreutils
# (ulimit is the shell's own).
# The PATH holds the stand-in curl, the tools it and download.sh run, and nothing else.
mkdir -p "$tmp/curl-only"; for c in bash cat mktemp rm tr; do ln -s "$(command -v "$c")" "$tmp/curl-only/$c"; done
ln -s "$tmp/bin/curl" "$tmp/curl-only/curl"
out_text="$(PATH="$tmp/curl-only" FAKE_CURL="200:$mirror/$pkg_sha" "$BASH" "$tools/lib/download.sh" "$url" "$tmp/curl-only/out" 268435456 2>&1)" &&
  status=0 || status=$?
if [[ "$status" == 0 && "$(sha "$tmp/curl-only/out")" == "$pkg_sha" ]]; then
  pass "download.sh needs no coreutils timeout, only curl and the shell"; else fail "curl alone (exit $status; $out_text)"; fi
# The size bound is rounded up to whole KiB for ulimit -f, so a body within <max-bytes> is accepted
# whatever the bound; also when POSIXLY_CORRECT starts bash in POSIX mode, whose ulimit -f counts
# 512-byte blocks (a 1024-byte limit here) unless download.sh turns that mode off.
printf '%01200d' 0 > "$tmp/small"
for posix in "" 1; do
  rm -f -- "$tmp/small-out"
  out_text="$(PATH="$tmp/bin:$PATH" FAKE_CURL="200:$tmp/small" env ${posix:+POSIXLY_CORRECT=1} "$BASH" "$tools/lib/download.sh" "$url" "$tmp/small-out" 1500 2>&1)" &&
    status=0 || status=$?
  if [[ "$status" == 0 && "$(wc -c < "$tmp/small-out")" -eq 1200 ]]; then
    pass "a bound that is not a whole number of KiB still accepts a body within it${posix:+, with POSIXLY_CORRECT=1}"
  else fail "rounded bound${posix:+ with POSIXLY_CORRECT=1} (exit $status; $out_text)"; fi
done
printf 'not the package' > "$tmp/wrong"
o="$tmp/wrong-download"; run "$o" "FAKE_CURL=200:$tmp/wrong"
expect_fail "a downloaded package with another hash fails" "from $url has SHA-256" "$o"
# A body of exactly the bound passes the size bound (and then fails only on its hash); one byte more
# is stopped.
truncate -s 268435456 "$tmp/at-bound"
o="$tmp/at-bound-download"; run "$o" "FAKE_CURL=200:$tmp/at-bound"
expect_fail "a download of exactly 256 MiB is not stopped by the size bound" "from $url has SHA-256" "$o"
rm -f "$tmp/at-bound"
truncate -s 268435457 "$tmp/huge" # 256 MiB and one byte
# Run in an empty directory, where the SIGXFSZ stop would leave a core file, with the soft core
# limit raised to the hard one, so that only download.sh's ulimit -c 0 can give curl a limit of 0.
o="$tmp/huge-download"; mkdir -p "$tmp/huge-cwd"; soft_core="$(ulimit -S -c)"
ulimit -S -c "$(ulimit -H -c)"; cd "$tmp/huge-cwd"; run "$o" "FAKE_CURL=200:$tmp/huge"; cd "$OLDPWD"
ulimit -S -c "$soft_core"
expect_line "with curl before 8.4, a download one byte over 256 MiB is stopped while it is written, and the one line names the size bound" \
  "fetch-driver: linux_amd64: downloading $url failed (curl exit 153): the body reached the 268435456-byte size bound (ulimit -f) and was stopped" "$o"
name="curl runs with a core-file limit of 0, so the size stop leaves no core file"
if [[ "$(ulimit -H -c)" == 0 ]]; then echo "ok $((n += 1)) - $name # SKIP the hard core-file limit is already 0"
elif [[ "$(cat "$FAKE_CURL_CORE_LIMIT")" == 0 && -z "$(ls -A "$tmp/huge-cwd")" ]]; then pass "$name"
else fail "core files (curl's limit: $(cat "$FAKE_CURL_CORE_LIMIT"); left: $(ls -A "$tmp/huge-cwd" | tr '\n' ' '))"; fi
# --max-filesize refuses a declared size over the bound and, from curl 8.4, stops a larger body,
# with curl's own exit and message.
o="$tmp/max-filesize"; too_big="curl: (63) Exceeded the maximum allowed file size (268435456) with 268435456 bytes"
run "$o" "FAKE_CURL=exit:63:$too_big"
expect_line "from curl 8.4, a body of undeclared size stopped at the bound gives one line with curl's exit 63 and message" \
  "fetch-driver: linux_amd64: downloading $url failed (curl exit 63): $too_big " "$o"
# A file-size limit that cannot be set, under a lower hard limit, is named, and curl does not run.
# A process that may raise its hard limit again (CAP_SYS_RESOURCE: root, or a privileged
# container) cannot make one that cannot be set, so the case is skipped there.
name="a file-size limit that cannot be set fails on one line before curl runs, blaming no curl exit"
if (ulimit -S -f 1024 && ulimit -H -f 1024 && ulimit -f 262144) 2>/dev/null; then
  echo "ok $((n += 1)) - $name # SKIP this process may raise its hard file-size limit again (CAP_SYS_RESOURCE)"
else
  o="$tmp/no-file-limit"; rm -f "$FAKE_CURL_ARGS"
  out_text="$(ulimit -S -f 1024 && ulimit -H -f 1024 && env -u RS_DRIVER_MIRROR RS_DRIVER_LOCK="$base_lock" PATH="$tmp/bin:$PATH" \
    FAKE_CURL=fail bash "$tools/fetch-driver.sh" linux_amd64 "$o" 2>&1)" && status=0 || status=$?
  if [[ ! -e "$FAKE_CURL_ARGS" ]]; then
    expect_line "$name" \
      "fetch-driver: linux_amd64: downloading $url failed: cannot set the file-size limit (ulimit -f) to 262144 KiB for the 268435456-byte bound" "$o"
  else fail "$name: curl ran (exit $status; $out_text)"; fi
fi
o="$tmp/failed-download"; run "$o"
expect_line "a failed download names curl's exit status and its message, on one line" \
  "fetch-driver: linux_amd64: downloading $url failed (curl exit 99): curl: (99) the stand-in fails " "$o"
# download.sh needs curl, and says so on one line.
mkdir -p "$tmp/no-curl"
out_text="$(PATH="$tmp/no-curl" "$BASH" "$tools/lib/download.sh" "$url" "$tmp/no-curl/out" 1 2>&1)" && status=0 || status=$?
if [[ "$status" == 1 && "$out_text" == "curl is required and was not found on PATH" ]]; then
  pass "without curl on PATH, download.sh fails on one line naming it"; else fail "no curl (exit $status; $out_text)"; fi
# The stand-in answers 302: a real 3xx would need a local HTTPS endpoint and a way to point the
# locked URL at it, which the fetch deliberately lacks.
o="$tmp/redirect"; run "$o" "FAKE_CURL=302:$tmp/wrong"
expect_fail "a redirect from the lock's URL fails and is not followed" "answered HTTP 302; redirects are not followed" "$o"

bad_lock="$tmp/off-path.lock"
sed 's|https://s3.amazonaws.com/redshift-downloads/|https://example.com/redshift-downloads/|' "$base_lock" > "$bad_lock"
o="$tmp/off-path"; run "$o" "RS_DRIVER_LOCK=$bad_lock" "FAKE_CURL=200:$mirror/$pkg_sha"
if [[ "$status" == 1 && "$out_text" == *"the lock failed check-lock.sh"* && ! -e "$FAKE_CURL_ARGS" ]] && no_set "$o"; then
  pass "an off-path URL fails before any download"; else fail "off-path URL (exit $status; $out_text)"; fi

other_lock="$tmp/other-driver.lock"
sed "s/$drv_sha/$(printf '%064d' 0)/" "$base_lock" > "$other_lock"
o="$tmp/other-driver"; run "$o" "RS_DRIVER_LOCK=$other_lock" "RS_DRIVER_MIRROR=$mirror"
expect_fail "a driver member other than the lock's fails the build" "librsodbc64.so has SHA-256 $drv_sha, the lock expects" "$o"

# elf_case <name> <ELF class> <byte order>: a stand-in package, a real RPM built here whose gzip
# payload holds the locked member (an x86-64 ELF header of that class and byte order), both locked
# by their hashes, so only the class and byte-order check can refuse it.
mkdir -p "$tmp/elf-mirror"
elf_case() {
  local e="$tmp/elf-$2-$3" p_sha m_sha
  python3 -I -c 'import sys; sys.stdout.buffer.write(b"\x7fELF" + bytes([int(sys.argv[1]), int(sys.argv[2]), 1]) +
    bytes(9) + bytes([2, 0, 62, 0]) + bytes(44))' "$2" "$3" > "$e.so"
  python3 -I "$here/make_cpio.py" "$e.cpio" "$path_in_pkg:file:1:$e.so"
  python3 -I "$here/make_rpm.py" "$e.rpm" AmazonRedshiftODBC-64-bit 2.3.0 0 x86_64 < "$e.cpio"
  p_sha="$(sha "$e.rpm")"; m_sha="$(sha "$e.so")"
  cp "$e.rpm" "$tmp/elf-mirror/$p_sha"; sed "s/$drv_sha/$m_sha/; s/$pkg_sha/$p_sha/" "$base_lock" > "$e.lock"
  run "$e.out" "RS_DRIVER_MIRROR=$tmp/elf-mirror" "RS_DRIVER_LOCK=$e.lock" "PATH=$tmp/bin:$PATH"
  expect_fail "$1" "$path_in_pkg is not a 64-bit little-endian ELF file" "$e.out"
}
elf_case "a 32-bit ELF driver fails" 1 1
elf_case "a big-endian ELF driver fails" 2 2

# The x86-64 package locked for linux_arm64 fails on the ELF machine, after its hash matched.
arm_lock="$tmp/arm.lock"
sed 's/^linux_amd64 /linux_arm64 /; /^linux_arm64 .*aarch64.rpm/d' "$base_lock" > "$arm_lock"
o="$tmp/machine"
out_text="$(env PATH="$tmp/bin:$PATH" FAKE_CURL=fail RS_DRIVER_MIRROR="$mirror" RS_DRIVER_LOCK="$arm_lock" \
  bash "$tools/fetch-driver.sh" linux_arm64 "$o" 2>&1)" && status=0 || status=$?
expect_fail "a driver for another ELF machine fails" "is ELF machine 62, not aarch64" "$o"

t="$tmp/notice-tree/third_party/redshift_odbc"; mkdir -p "$t"
cp -R "$repo/third_party/redshift_odbc/tools" "$repo/third_party/redshift_odbc/policy" "$repo/third_party/redshift_odbc/driver.lock" "$t/"
cp "$repo/third_party/redshift_odbc/"{LICENSE,NOTICE} "$t/"; printf 'changed\n' > "$t/THIRD_PARTY_LICENSES"
o="$tmp/notice"; run "$o" "RS_DRIVER_MIRROR=$mirror" -- "$t/tools"
expect_fail "a changed checked-in notice fails, naming it" "notice THIRD_PARTY_LICENSES has SHA-256" "$o"
# driver_info.h is generated last, by driver_info.py; a failure there fails the fetch and leaves no
# header (the set's files, moved in just before, stay).
t="$tmp/policy-tree/third_party/redshift_odbc"; mkdir -p "$t"
cp -R "$repo/third_party/redshift_odbc/tools" "$repo/third_party/redshift_odbc/policy" "$repo/third_party/redshift_odbc/driver.lock" "$t/"
cp "$repo/third_party/redshift_odbc/"{LICENSE,NOTICE,THIRD_PARTY_LICENSES} "$t/"
printf 'PG-HOST  Host\n' >> "$t/policy/known-environment.txt"
o="$tmp/bad-policy"; run "$o" "RS_DRIVER_MIRROR=$mirror" -- "$t/tools"
if [[ "$status" == 1 && "$out_text" == *"driver_info.h cannot be generated"* && ! -e "$o/driver_info.h" ]] && no_work "$o"; then
  pass "a header that cannot be generated fails the fetch, naming it, and leaves no header"
else fail "header generation failure (exit $status; $out_text)"; fi

# The lock is read once: a stand-in od, which the ELF check runs after the lock has been read,
# overwrites the lock file with one that fails check-lock.sh, and the header is still generated
# from the copy read before.
mkdir -p "$tmp/clobber-bin"
printf '%s\n' '#!/usr/bin/env bash' "printf 'not a lock\\n' > \"\$CLOBBER_LOCK\"" "exec $(command -v od) \"\$@\"" \
  > "$tmp/clobber-bin/od"; chmod +x "$tmp/clobber-bin/od"
cp "$base_lock" "$tmp/clobbered.lock"
o="$tmp/read-once"; run "$o" "RS_DRIVER_MIRROR=$mirror" "RS_DRIVER_LOCK=$tmp/clobbered.lock" "CLOBBER_LOCK=$tmp/clobbered.lock" \
  "PATH=$tmp/clobber-bin:$tmp/bin:$PATH"
if [[ "$status" == 0 ]] && set_ok "$o" && [[ "$(cat "$tmp/clobbered.lock")" == "not a lock" ]]; then
  pass "the lock is read once, so a lock changed during the run does not reach the header"
else fail "lock read once (exit $status; $out_text)"; fi

o="$tmp/stale"; mkdir -p "$o"; echo old > "$o/librsodbc64.so"; echo old > "$o/driver_info.h"
run "$o" "RS_DRIVER_MIRROR=$tmp/bad-mirror"
expect_fail "a failure removes an earlier run's set and header" "the lock expects $pkg_sha" "$o"
o="$tmp/stale-lock"; mkdir -p "$o"; echo old > "$o/driver_info.h"; printf 'not a lock\n' > "$tmp/junk.lock"
run "$o" "RS_DRIVER_LOCK=$tmp/junk.lock"
if [[ "$status" == 1 && "$out_text" == *"the lock failed check-lock.sh"* && ! -e "$o/driver_info.h" ]]; then
  pass "a lock that fails the checker still removes an earlier header"; else fail "stale header on a bad lock (exit $status; $out_text)"; fi

# The 2.3.0.0 package holds symbolic links under ./usr/lib/.build-id/; one locked as the driver's
# path must be refused, and nothing written for it.
# The first member under ./usr/lib/.build-id/ that is a symbolic link, read from the newc headers.
link_path="$(python3 -I "$tools/lib/rpm.py" --payload "$mirror/$pkg_sha" | python3 -I -c '
import stat, sys
f = sys.stdin.buffer
while True:
    h = f.read(110); mode, size, nsize = int(h[14:22], 16), int(h[54:62], 16), int(h[94:102], 16)
    name = f.read(nsize).rstrip(b"\0").decode(); f.read((4 - (110 + nsize) % 4) % 4)
    if name == "TRAILER!!!": break
    if stat.S_ISLNK(mode) and name.startswith("./usr/lib/.build-id/"): print(name); break
    f.read(size + (4 - size % 4) % 4)')"
if [[ -n "$link_path" ]]; then
  sed "s|^\(linux_amd64 .*\) \./opt/amazon/redshiftodbcx64/librsodbc64\.so$|\1 $link_path|" \
    "$base_lock" > "$tmp/link.lock"
  o="$tmp/link"; run "$o" "RS_DRIVER_MIRROR=$mirror" "RS_DRIVER_LOCK=$tmp/link.lock"
  expect_fail "a symbolic link at the locked path is refused" "is not a regular file (mode 120777)" "$o"
else fail "the locked package holds a symbolic link under ./usr/lib/.build-id/"; fi

# without <tool> [env...]: runs fetch-driver.sh with a PATH of every command the script and its
# helpers run, but not <tool>, into an out-dir holding an earlier header, which must go too, even
# though these failures come before the work directory exists.
without() {
  local tool="$1" d="$tmp/no-$1" c; shift
  mkdir -p "$d" "$d-out"; echo old > "$d-out/driver_info.h"
  for c in bash env python3 awk od tr cp mv mkdir mktemp rm basename dirname; do
    [[ "$c" == "$tool" ]] || ln -s "$(command -v "$c")" "$d/$c"
  done
  out_text="$(env -u RS_DRIVER_MIRROR RS_DRIVER_LOCK="$base_lock" PATH="$d" "$@" "$(command -v bash)" "$tools/fetch-driver.sh" linux_amd64 "$d-out" 2>&1)" &&
    status=0 || status=$?
  expect_fail "a missing $tool fails with its name" "$tool is required and was not found on PATH" "$d-out"
}
without python3 "RS_DRIVER_MIRROR=$mirror"
without curl # no mirror and no kept copy, so the package must be downloaded

# macOS.
skip() { n=$((n+1)); echo "ok $n - $1 # SKIP $2"; }
mac_row="$(bash "$tools/check-lock.sh" "$base_lock" | awk '$1 == "driver" && $2 == "osx_arm64"')"
read -r _ _ mac_name mac_sha _ mac_url mac_pkg_sha mac_path <<<"$mac_row"
# run_mac <platform> <out> [env...]: as run, for a macOS platform.
run_mac() {
  local p="$1" o="$2"; shift 2
  rm -f "$FAKE_CURL_ARGS"
  out_text="$(env -u RS_DRIVER_MIRROR RS_DRIVER_LOCK="$base_lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail "$@" \
    bash "$tools/fetch-driver.sh" "$p" "$o" 2>&1)" && status=0 || status=$?
}
no_mac_set() { [[ ! -e "$1/driver_info.h" && ! -e "$1/$mac_name" && ! -L "$1/$mac_name" && ! -e "$1/LICENSE" ]]; }
expect_mac_fail() {  # expect_mac_fail <name> <substring> <out>
  if [[ "$status" == 1 && "$out_text" == *"$2"* ]] && no_mac_set "$3"; then pass "$1"; else
    fail "$1 (exit $status; output: $out_text)"; fi
}
if [[ -z "$mac_row" ]]; then
  skip "macOS fetch" "the lock under test has no osx_arm64 row"
else
  # A mirror file that is not the package needs no real package, so it always runs.
  # An earlier header in <out-dir> is removed before anything else, so a failure leaves none.
  o="$tmp/mac-tampered"; mkdir -p "$tmp/bad-mirror" "$o"; printf 'not the package' > "$tmp/bad-mirror/$mac_pkg_sha"
  echo old > "$o/driver_info.h"
  run_mac osx_arm64 "$o" "RS_DRIVER_MIRROR=$tmp/bad-mirror"
  expect_mac_fail "a macOS package hash mismatch fails, naming the platform, the file and both hashes, and removes an earlier header" \
    "osx_arm64: package ${mac_url##*/} from RS_DRIVER_MIRROR has SHA-256 $(sha "$tmp/bad-mirror/$mac_pkg_sha"), the lock expects $mac_pkg_sha" "$o"
  # shellcheck source=release-cache.sh
  source "$here/release-cache.sh"
  if real_mac="$(release_file "$mac_pkg_sha" "$mac_url" "$mac_pkg_sha")"; then
    run_mac osx_amd64 "$tmp/osx_amd64" "RS_DRIVER_MIRROR=${real_mac%/*}"; amd="$status $out_text"
    run_mac osx_arm64 "$tmp/osx_arm64" "RS_DRIVER_MIRROR=${real_mac%/*}"
    if [[ "$amd" == "0 "* && "$status" == 0 && "$(sha "$tmp/osx_arm64/$mac_name")" == "$mac_sha" ]] &&
      same "$tmp/osx_amd64/$mac_name" "$tmp/osx_arm64/$mac_name" && [[ ! -e "$FAKE_CURL_ARGS" ]] &&
      grep -q '"osx_amd64"' "$tmp/osx_amd64/driver_info.h" && grep -q "X(driver, \"$tmp/osx_arm64/$mac_name\")" "$tmp/osx_arm64/driver_info.h"; then
      pass "both macOS platforms take the same universal library, equal to the lock, from the locked package"
    else fail "macOS fetch (osx_amd64: $amd; osx_arm64: $status $out_text)"; fi
  else fail "the locked macOS package ${mac_url##*/} is in neither RS_DRIVER_MIRROR nor the cache, and its download failed"; fi
fi

# The member checks through fetch-driver.sh: test packages at the lock's path, each locked by
# its own hash in a copy of the lock and served from a mirror.
# mac_fixture <json spec> [<driver-sha256>]: writes the package, serves it from $tmp/mac-mirror,
# and writes $tmp/fixture.lock, the lock with both macOS rows naming it (and the driver hash).
mac_fixture() {
  local p="$tmp/fixture.pkg" s m="$tmp/mac-mirror"
  python3 -I "$here/make_xar.py" "$p" "$1"
  s="$(sha "$p")"; mkdir -p "$m"; cp "$p" "$m/$s"
  awk -v s="$s" -v d="${2:-}" '$1 ~ /^osx_/ && $2 == "driver" { $6 = s; if (d != "") $4 = d } { print }' \
    "$base_lock" > "$tmp/fixture.lock"
}
# mac_case <name> <substring> <json spec>
mac_case() {
  mac_fixture "$3"
  o="$tmp/mac-fixture"; rm -rf -- "${o:?}"
  run_mac osx_arm64 "$o" "RS_DRIVER_MIRROR=$tmp/mac-mirror" "RS_DRIVER_LOCK=$tmp/fixture.lock"
  expect_mac_fail "$1" "$2" "$o"
}
if [[ -n "$mac_row" ]]; then
  dylib="${mac_path#./*/Payload/}"
  # A test package whose member starts as a universal file does, locked with its hash: both
  # platforms take it, and one changed byte fails the lock's SHA-256 check.
  printf '\xca\xfe\xba\xbe universal library stand-in' > "$tmp/universal"
  mac_fixture "{\"members\": [[\"./$dylib\", \"file\", 1, \"@$tmp/universal\"]]}" "$(sha "$tmp/universal")"
  run_mac osx_arm64 "$tmp/fx-arm64" "RS_DRIVER_MIRROR=$tmp/mac-mirror" "RS_DRIVER_LOCK=$tmp/fixture.lock"; arm="$status $out_text"
  run_mac osx_amd64 "$tmp/fx-amd64" "RS_DRIVER_MIRROR=$tmp/mac-mirror" "RS_DRIVER_LOCK=$tmp/fixture.lock"
  if [[ "$arm" == "0 "* && "$status" == 0 ]] && same "$tmp/universal" "$tmp/fx-arm64/$mac_name" &&
    same "$tmp/universal" "$tmp/fx-amd64/$mac_name" && [[ ! -e "$FAKE_CURL_ARGS" ]] &&
    grep -q '"osx_arm64"' "$tmp/fx-arm64/driver_info.h" && grep -q "X(driver, \"$tmp/fx-amd64/$mac_name\")" "$tmp/fx-amd64/driver_info.h"; then
    pass "both macOS platforms take a locked test package's library unchanged, with driver_info.h"
  else fail "macOS fetch of a test package (osx_arm64: $arm; osx_amd64: $status $out_text)"; fi
  printf '\xca\xfe\xba\xbe universal library stand-iN' > "$tmp/changed"
  mac_fixture "{\"members\": [[\"./$dylib\", \"file\", 1, \"@$tmp/changed\"]]}" "$(sha "$tmp/universal")"
  o="$tmp/fx-changed"; run_mac osx_arm64 "$o" "RS_DRIVER_MIRROR=$tmp/mac-mirror" "RS_DRIVER_LOCK=$tmp/fixture.lock"
  expect_mac_fail "one changed byte in the macOS library fails the lock's SHA-256 check" \
    "$mac_name has SHA-256 $(sha "$tmp/changed"), the lock expects $(sha "$tmp/universal")" "$o"
  mac_case "a symbolic link at the macOS path is refused" "is not a regular file (mode 120777)" "{\"members\": [[\"./$dylib\", \"link\"]]}"
  mac_case "a device at the macOS path is refused" "is not a regular file" "{\"members\": [[\"./$dylib\", \"dev\"]]}"
  mac_case "two members at the macOS path are refused" "more than one member" "{\"members\": [[\"./$dylib\", \"file\"], [\"./$dylib\", \"file\"]]}"
  mac_case "a ../ member in the macOS payload is refused" "member b'../escape' has an absolute path or a .. segment" \
    "{\"members\": [[\"./$dylib\", \"file\"], [\"../escape\", \"file\"]]}"
  mac_case "a macOS member that is not a universal Mach-O file is refused" "is not a universal Mach-O file" "{\"members\": [[\"./$dylib\", \"file\"]]}"
  mac_case "a package xar.py refuses fails the fetch at once, naming the path" "osx_arm64: cannot take $mac_path from the package" \
    '{"break": ["payload-checksum"]}'
  # A macOS fetch needs python3 and nothing else: the locked test package, from the mirror.
  mac_fixture "{\"members\": [[\"./$dylib\", \"file\", 1, \"@$tmp/universal\"]]}" "$(sha "$tmp/universal")"
  for tool in python3 none; do
    d="$tmp/mac-no-$tool"; mkdir -p "$d"
    for c in bash env python3 awk od tr cp mv mkdir mktemp rm basename dirname; do
      [[ "$c" == "$tool" ]] || ln -s "$(command -v "$c")" "$d/$c"
    done
    o="$tmp/mac-out-no-$tool"; rm -rf -- "${o:?}"
    out_text="$(env RS_DRIVER_MIRROR="$tmp/mac-mirror" RS_DRIVER_LOCK="$tmp/fixture.lock" PATH="$d" "$(command -v bash)" \
      "$tools/fetch-driver.sh" osx_arm64 "$o" 2>&1)" && status=0 || status=$?
    if [[ "$tool" == python3 ]]; then
      expect_mac_fail "a macOS fetch without python3 fails with its name" "python3 is required and was not found on PATH" "$o"
    elif [[ "$status" == 0 ]] && same "$tmp/universal" "$o/$mac_name"; then
      pass "a macOS fetch needs only python3 and the shell's tools"
    else fail "a macOS fetch with only python3 (exit $status; $out_text)"; fi
  done
  # signer.txt must have one checksum row: a tree whose signer.txt has none fails with the policy
  # file's name, before the package is read.
  t="$tmp/signer-tree"; rm -rf -- "${t:?}"; mkdir -p "$t"; cp -R "$tools/.." "$t/redshift_odbc"
  sed -i.bak '/^checksum/d' "$t/redshift_odbc/policy/macos/signer.txt"
  mac_fixture "{\"members\": [[\"./$dylib\", \"file\", 1, \"@$tmp/universal\"]]}" "$(sha "$tmp/universal")"
  o="$tmp/mac-no-checksum"; rm -rf -- "${o:?}"
  out_text="$(env RS_DRIVER_MIRROR="$tmp/mac-mirror" RS_DRIVER_LOCK="$tmp/fixture.lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
    bash "$t/redshift_odbc/tools/fetch-driver.sh" osx_arm64 "$o" 2>&1)" && status=0 || status=$?
  expect_mac_fail "a signer.txt without a checksum row fails the macOS fetch, naming it" \
    "osx_arm64: policy: policy/macos/signer.txt must have exactly one checksum row" "$o"
  # The algorithm comes from signer.txt: one naming md5 refuses this sha1 package's payload. The
  # rows are set by appending and by s/^checksum .*/, as BSD sed has no \n in a replacement.
  echo 'checksum md5' >> "$t/redshift_odbc/policy/macos/signer.txt"
  o="$tmp/mac-md5"; rm -rf -- "${o:?}"
  out_text="$(env RS_DRIVER_MIRROR="$tmp/mac-mirror" RS_DRIVER_LOCK="$tmp/fixture.lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
    bash "$t/redshift_odbc/tools/fetch-driver.sh" osx_arm64 "$o" 2>&1)" && status=0 || status=$?
  expect_mac_fail "the macOS fetch reads its checksum algorithm from signer.txt" "archived checksum uses the algorithm 'sha1', which is not accepted" "$o"
  # A checksum row with two algorithms fails the fetch, as it fails verification (lib/common.sh
  # signer_checksum, which both call, refuses it).
  sed -i.bak 's/^checksum .*/checksum sha1 sha256/' "$t/redshift_odbc/policy/macos/signer.txt"
  o="$tmp/mac-two-algs"; rm -rf -- "${o:?}"
  out_text="$(env RS_DRIVER_MIRROR="$tmp/mac-mirror" RS_DRIVER_LOCK="$tmp/fixture.lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
    bash "$t/redshift_odbc/tools/fetch-driver.sh" osx_arm64 "$o" 2>&1)" && status=0 || status=$?
  expect_mac_fail "a signer.txt checksum row with two algorithms fails the macOS fetch" \
    "osx_arm64: policy: policy/macos/signer.txt must have exactly one checksum row" "$o"
  # A missing signer.txt fails with policy_lines' one message, not a second about the checksum row.
  rm -- "$t/redshift_odbc/policy/macos/signer.txt"
  o="$tmp/mac-no-signer"; rm -rf -- "${o:?}"
  out_text="$(env RS_DRIVER_MIRROR="$tmp/mac-mirror" RS_DRIVER_LOCK="$tmp/fixture.lock" PATH="$tmp/bin:$PATH" FAKE_CURL=fail \
    bash "$t/redshift_odbc/tools/fetch-driver.sh" osx_arm64 "$o" 2>&1)" && status=0 || status=$?
  if [[ "$out_text" == *"checksum row"* ]]; then fail "a missing signer.txt (a second message: $out_text)"; else
    expect_mac_fail "a missing signer.txt fails the macOS fetch with one message, naming it" \
      "osx_arm64: policy: policy/macos/signer.txt is missing" "$o"
  fi
  rm -rf -- "${t:?}"
  # The payload's checksums must use signer.txt's algorithm, as in verification.
  mac_case "a macOS package whose payload checksums use another algorithm is refused" "which is not accepted" \
    "{\"payload_alg\": \"md5\", \"members\": [[\"./$dylib\", \"file\", 1, \"@$tmp/universal\"]]}"
fi

# CMake cannot see a module a tool loads by path, so the fetch's DEPENDS names each one the
# platform's readers load, or an edit to it would not fetch again. Read from sources.cmake.
unlisted="$(python3 -I - "$repo" <<'PY'
import re, sys
repo, lib = sys.argv[1], sys.argv[1] + "/third_party/redshift_odbc/tools/lib/"
text = open(repo + "/src/transport/driver/sources.cmake").read()
fetch = text[text.index("set(redshift_fetch_depends)"):text.index('COMMENT "Fetching')]
shared = fetch[fetch.index("endif()"):]
branches = re.split(r'(?:else)?if\(redshift_object_format STREQUAL "(\w+)"\)', fetch[:fetch.index("endif()")])
readers = {"macho": ["xar.py"], "pe": ["msi.py", "rename_apply.py"]}
problems = []
for fmt, body in zip(branches[1::2], branches[2::2]):
    listed = set(re.findall(r'tools/lib/(\w+\.py)"', body + shared))
    todo, loaded = list(readers.pop(fmt)), set()
    while todo:
        name = todo.pop()
        if name not in loaded:
            loaded.add(name)
            todo += [m + ".py" for m in re.findall(r'\b(?:load|sibling)\("(\w+)"\)', open(lib + name).read())]
    problems += ["%s:%s" % (fmt, m) for m in sorted(loaded - listed)]
print(" ".join(problems + ["no-branch:" + f for f in sorted(readers)]))
PY
)"
name="each module the Windows and macOS readers load is in the fetch's DEPENDS"
if [[ -z "$unlisted" ]]; then pass "$name"; else fail "$name (not listed: $unlisted)"; fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
