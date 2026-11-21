#!/usr/bin/env bash
# Tool tests for third_party/redshift_odbc/tools/lib/cpio.py. Each case writes a small newc archive
# with a standard-library writer, including member types a normal user cannot create, and checks
# the exit status, the message and what was written. Run by run-tool-tests.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
reader="$here/../../../third_party/redshift_odbc/tools/lib/cpio.py"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT

# archive <file> <member>...: a newc archive by make_cpio.py, the member checks' fixture writer (see there).
archive() { python3 -I "$here/make_cpio.py" "$@"; }

n=0; failed=0
# verdict <name> <expected-exit> <expected-substring>: after a run that set out and status. A failure
# writes nothing, not even the private temporary (.cpio.*) the data waits in; a success leaves only
# <out-file>.
verdict() {
  n=$((n+1))
  if [[ "$status" == "$2" && "$out" == *"$3"* ]] && { [[ "$2" == 0 ]] || [[ ! -e "$tmp/out" ]]; } &&
    ! compgen -G "$tmp/.cpio.*" >/dev/null; then
    echo "ok $n - $1"
  else
    echo "not ok $n - $1 (exit $status, wanted $2; output: $out; left: $(cd "$tmp" && echo .cpio.*))"
    failed=$((failed+1))
  fi
}
check() {  # check <name> <expected-exit> <expected-substring> <member-path> <archive-member>...
  local name="$1" want="$2" needle="$3" path="$4"
  shift 4
  archive "$tmp/a.cpio" "$@"; rm -f "$tmp/out"
  out="$(python3 -I "$reader" "$path" "$tmp/out" < "$tmp/a.cpio" 2>&1)" && status=0 || status=$?
  verdict "$name" "$want" "$needle"
}

check "takes the one regular file at the path" 0 "" ./opt/x.so ./usr:dir ./opt/x.so:file ./opt/y:link
[[ "$(cat "$tmp/out")" == "data of ./opt/x.so" ]] && echo "ok $((n += 1)) - writes that member's data" ||
  { echo "not ok $((n += 1)) - writes that member's data"; failed=$((failed+1)); }
check "refuses a symbolic link at the path" 1 "is not a regular file (mode 120777)" ./opt/x.so ./opt/x.so:link
check "refuses a directory at the path" 1 "is not a regular file (mode 40755)" ./opt/x.so ./opt/x.so:dir
check "refuses a FIFO at the path" 1 "is not a regular file" ./opt/x.so ./opt/x.so:fifo
check "refuses a device at the path" 1 "is not a regular file" ./opt/x.so ./opt/x.so:dev
check "refuses a hard-linked file at the path" 1 "has 2 links, not one" ./opt/x.so ./opt/x.so:file:2
check "refuses two members at the path, writing nothing" 1 "more than one member" ./opt/x.so ./opt/x.so:file ./opt/x.so:file
check "refuses an archive without the path" 1 "no member at ./opt/x.so" ./opt/x.so ./opt/y.so:file
check "refuses an absolute member, even after the path was taken" 1 "member b'/etc/x' has an absolute path" ./opt/x.so ./opt/x.so:file /etc/x:file
check "refuses a member with a .. segment" 1 "member b'../escape' has an absolute path or a .. segment" ./opt/x.so ../escape:file ./opt/x.so:file
check "refuses a .. segment inside a member's path" 1 "member b'./a/../b' has" ./opt/x.so ./a/../b:file ./opt/x.so:file
# Every spelling of the path names one member: a second member spelled another way is refused, as
# an RPM payload never holds one, and the path asked for may be written any way.
for alias in opt/x.so ./opt/./x.so ./opt//x.so; do
  check "refuses a member spelled $alias beside ./opt/x.so" 1 "member b'$alias' is not spelled ./<path>" ./opt/x.so ./opt/x.so:file "$alias:file"
done
check "accepts the . root directory a macOS payload starts with" 0 "" ./opt/x.so .:dir ./opt/x.so:file
check "refuses a ./ member, whose second segment is empty" 1 "member b'./' is not spelled ./<path>" ./opt/x.so ./:dir ./opt/x.so:file
for spelling in opt/x.so ./opt/./x.so ./opt//x.so; do
  check "takes ./opt/x.so when asked for $spelling" 0 "" "$spelling" ./opt/x.so:file
done
check "refuses the crc format" 1 "crc format, which is not accepted" ./opt/x.so '!crc' ./opt/x.so:file
check "refuses a header field that is not hexadecimal" 1 "not hexadecimal" ./opt/x.so '!badhex' ./opt/x.so:file
check "refuses a name without its NUL" 1 "does not end with exactly one NUL" ./opt/x.so '!nonul' ./opt/x.so:file
check "refuses a NUL inside a name" 1 "does not end with exactly one NUL" ./opt/x.so '!innernul' ./opt/x.so:file
check "refuses a magic other than newc's" 1 "not a newc cpio archive" ./opt/x.so '!magic' ./opt/x.so:file
check "refuses a name size of 0" 1 "name size 0 is out of bounds" ./opt/x.so '!zeroname'
long="./$(printf 'a%.0s' {1..4094})" # 4096 bytes, so 4097 with its NUL, one over the bound
check "refuses a name over 4096 bytes with its NUL" 1 "name size 4097 is out of bounds" ./opt/x.so "$long:file" ./opt/x.so:file
check "refuses a member declared over 1 GiB, before reading its data" 1 \
  "member b'./opt/big' declares 1073741825 bytes, more than the 1073741824" ./opt/x.so '!declare=1073741825'
check "refuses more than 100000 members" 1 "more than 100000 members" ./opt/x.so '!many=100001' ./opt/x.so:file
# The total bound, 4 GiB, crossed for real: four 1 GiB members streamed through a pipe, which takes
# about a second and writes nothing to disk.
rm -f "$tmp/out"
out="$(python3 -I "$reader" ./opt/x.so "$tmp/out" 2>&1 < <(archive - '!total' ./opt/x.so:file 2>/dev/null))" && status=0 || status=$?
verdict "refuses members holding more than 4 GiB in all" 1 "members hold more than 4294967296 bytes"
check "refuses data after the trailer, writing nothing" 1 "data follows the archive's trailer" ./opt/x.so '!after' ./opt/x.so:file
check "refuses a truncated archive" 1 "ends inside a member" ./opt/x.so '!short' ./opt/x.so:file
check "refuses a trailer that is not empty" 1 "trailer is not empty" ./opt/x.so '!trailer' ./opt/x.so:file
check "refuses more than a block of padding after the trailer" 1 "more than 4096 bytes follow" ./opt/x.so '!padding' ./opt/x.so:file
archive "$tmp/a.cpio" ./opt/x.so:file; : > "$tmp/out"
out="$(python3 -I "$reader" ./opt/x.so "$tmp/out" < "$tmp/a.cpio" 2>&1)" && status=0 || status=$?
[[ "$status" == 1 && "$out" == *"File exists"* && ! -s "$tmp/out" ]] && ! compgen -G "$tmp/.cpio.*" >/dev/null &&
  echo "ok $((n += 1)) - never writes over an existing file" ||
  { echo "not ok $((n += 1)) - never writes over an existing file ($status: $out)"; failed=$((failed+1)); }

# --find lists the members whose last segment is a given name, of any type, and writes nothing.
find_case() {  # find_case <name> <expected-exit> <expected-output> <archive-member>...
  local name="$1" want="$2" needle="$3" out status
  shift 3; n=$((n+1))
  archive "$tmp/a.cpio" "$@"
  out="$(cd "$tmp" && python3 -I "$reader" --find LICENSE NOTICE < "$tmp/a.cpio" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$want" && "$out" == "$needle" ]]; then echo "ok $n - $name"; else
    echo "not ok $n - $name (exit $status, wanted $want; output: $out)"; failed=$((failed+1)); fi
}
find_case "--find lists each match with its type unchecked" 0 $'LICENSE ./opt/a/LICENSE\nNOTICE ./d/NOTICE\nLICENSE ./LICENSE' \
  ./opt/a/LICENSE:file ./opt/a/LICENSES:file ./d/NOTICE:link ./LICENSE:dir
find_case "--find prints nothing when no member matches" 0 "" ./opt/x.so:file
find_case "--find still refuses a .. member" 1 "cpio.py: member b'../LICENSE' has an absolute path or a .. segment" ./opt/x.so:file ../LICENSE:file
find_case "--find refuses a matching path with a space" 1 "cpio.py: member b'./a b/LICENSE' has a space or a byte that is not printable ASCII" "./a b/LICENSE:file"

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
