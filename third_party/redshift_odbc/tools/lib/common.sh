# The helpers the tools share, sourced by tools/fetch-driver.sh, lib/verify_linux.sh,
# lib/verify_macos.sh, lib/linux_signer.sh, lib/macos_signer.sh and lib/elf_policy.sh, each of
# which defines fail first (this file calls it).
#
#   sha256 <file>               prints the file's hex SHA-256, read in 1 MiB pieces so a large
#                               file is never held in memory whole; prints one line and fails if
#                               it cannot be read
#   size_of <file>              prints the file's size in bytes
#   policy_lines <dir> <name>   prints <dir>/<name> without comments, blank lines or repeated
#                               spaces, or fails "policy: <name> is missing" or "is unreadable"
#   policy_row <rows> <key>     prints the rest of the one row of <rows> (policy_lines' output)
#                               that starts with <key>; returns 1 if there is not exactly one, or
#                               it has no value (signer.txt's rows, for signer_checksum and
#                               lib/verify_macos.sh's row())
#   signer_checksum <odbc-dir>  prints the algorithm of the one checksum row of
#                               <odbc-dir>/policy/macos/signer.txt, a lowercase name such as sha1,
#                               or fails as policy_lines does or "policy: policy/macos/signer.txt
#                               must have exactly one checksum row, "checksum <algorithm>"" (the
#                               fetch, the macOS verification and the pkgutil suite)
#   no_trust_variables          unsets the variables that would add a trusted certificate to
#                               openssl, or load other configuration or code into it or into the
#                               OpenSSL that Python's hashlib uses
#
# The openssl rules both signers share (lib/linux_signer.sh, lib/macos_signer.sh):
#   check_openssl               the openssl rule: openssl is on PATH, runs, and is OpenSSL 1.1.0 or
#                               later, the first with the verify options the chain rules need; an
#                               older one would refuse every release for the wrong reason
#   store_flags                 sets the array store to the openssl verify flags that keep the
#                               default CA directory out (-no-CApath) and, on OpenSSL 3, the default
#                               certificate store (-no-CAstore), so only the -CAfile a rule passes
#                               is trusted
#   chain_shape <output>        prints the chain openssl verify -show_chain reported, one
#                               depth=<n><u|t> word per certificate, u for one from the untrusted
#                               list and t for one from the trusted file
#
# The release files both verifications read (lib/verify_linux.sh, lib/verify_macos.sh), which use
# the caller's platform in their messages and share two variables, mirror and want_package_sha:
#   retained <version> <package-url> <platform>...
#                               sets mirror and want_package_sha. When RS_DRIVER_MIRROR is set and
#                               the recorded lock in RS_DRIVER_LOCK names <version> and a driver row
#                               of one of <platform>..., mirror is RS_DRIVER_MIRROR and
#                               want_package_sha that row's package SHA-256, and the row's URL must
#                               be <package-url>; otherwise both are empty and the run says why it
#                               downloads (mirror)
#   get <url> <retained-name> <out> <max-bytes>
#                               one release file into <out>: the regular file <retained-name> in
#                               mirror, within the bound, when mirror is set, and otherwise <url>
#                               through lib/download.sh (mirror, download)
#   notices                     the notices the lock checker requires, an array
#   notice_rows <version> <odbc-dir> <work> <found> [<notice-dir> <take>]
#                               sets rows to the notice rows: each notice from the release tag
#                               (or its retained copy), bounded to 1 MiB, equal to the checked-in
#                               copy. <found> lists the package's members named like a notice, as
#                               "<name> <path>" lines. The release tag's copy is the one that ships:
#                               a package may hold each notice once, at <notice-dir>/<name> in its
#                               exact spelling, where the command <take> <path> <out-file> takes it
#                               out as a regular file with exactly the release tag's bytes; any
#                               other member named like a notice fails the run, named, as does a
#                               call without <notice-dir> (notices).
#                               <version> must be <a>.<b>.<c>.0, whose tag is v<a>.<b>.<c>: the tag
#                               drops the last part, so 2.3.0.5 would take 2.3.0's notices. Both
#                               verifications refuse any other version first (no release-tag rule)
#
# python3 runs with -I, so neither PYTHON* variables nor a module beside the current directory can
# change what it imports. It sets no shell option: a sourced file shares its caller's shell, and
# the callers set their own (set -euo pipefail) before they source it.
# shellcheck shell=bash

# This file's directory, without running dirname, as the callers source it by its absolute path
# and some run with no more on PATH than the tools their rule names.
common_lib="${BASH_SOURCE[0]%/*}"
notices=(LICENSE NOTICE THIRD_PARTY_LICENSES)

sha256() {
  python3 -I -c 'import hashlib,sys
h = hashlib.sha256()
try:
    with open(sys.argv[1], "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
except OSError as e:
    # On stdout, so a caller that captures the output can name its rule and this reason together.
    print("cannot read %s: %s" % (sys.argv[1], e.strerror)); sys.exit(1)
print(h.hexdigest())' "$1"
}
size_of() { python3 -I -c 'import os,sys; print(os.stat(sys.argv[1]).st_size)' "$1"; }
policy_lines() {
  [[ -f "$1/$2" ]] || fail "policy: $2 is missing"
  [[ -r "$1/$2" ]] || fail "policy: $2 is unreadable"
  # In the C locale, as a policy file is ASCII: macOS's awk stops on a byte its locale cannot decode.
  LC_ALL=C awk '{ sub(/#.*/, "") } NF { $1 = $1; print }' "$1/$2"
}
policy_row() {
  LC_ALL=C awk -v k="$2" '$1 == k { n++; sub(/^[^ ]+ ?/, ""); v = $0 } END { if (n != 1 || v == "") exit 1; print v }' <<<"$1"
}
signer_checksum() {
  local rows alg
  rows="$(policy_lines "$1" policy/macos/signer.txt)" || exit 1 # policy_lines has said why
  alg="$(policy_row "$rows" checksum)" || alg=""
  [[ "$alg" =~ ^[a-z0-9]+$ ]] ||
    fail "policy: policy/macos/signer.txt must have exactly one checksum row, \"checksum <algorithm>\""
  printf '%s\n' "$alg"
}
no_trust_variables() {
  unset SSL_CERT_FILE SSL_CERT_DIR SSL_CERT_URI OPENSSL_CONF OPENSSL_CONF_INCLUDE OPENSSL_MODULES OPENSSL_ENGINES
}

check_openssl() {
  local text
  command -v openssl >/dev/null || fail "openssl: openssl is required and was not found on PATH"
  # An openssl that does not run (a missing library, say) fails here with its own message.
  text="$(openssl version 2>&1)" || fail "openssl: openssl on PATH does not run: $(printf '%s' "$text" | tr '\n' ' ')"
  text="${text%%$'\n'*}"
  [[ "$text" =~ ^OpenSSL\ ([0-9]+)\.([0-9]+)\. ]] &&
    ((BASH_REMATCH[1] > 1 || (BASH_REMATCH[1] == 1 && BASH_REMATCH[2] >= 1))) ||
    fail "openssl: verification needs OpenSSL 1.1.0 or later, and openssl on PATH is: $text"
}
store_flags() {
  local help
  store=(-no-CApath)
  # The help text is read whole first: under pipefail, grep -q leaving early could fail the probe.
  help="$(openssl verify -help 2>&1 || true)"
  if [[ "$help" == *-no-CAstore* ]]; then store+=(-no-CAstore); fi
}
chain_shape() {
  printf '%s\n' "$1" | awk '/^depth=/ { printf "%s%s ", substr($1, 1, length($1) - 1), (/ \(untrusted\)$/ ? "u" : "t") }'
}

retained() {
  local version="$1" url="$2" why="" lock row row_platform lock_url names
  shift 2
  names="$*"
  mirror="" want_package_sha=""
  [[ -n "${RS_DRIVER_MIRROR+set}" ]] || return 0
  # An empty RS_DRIVER_MIRROR is an error, not a silent fall back to the download, as an empty
  # RS_DRIVER_LOCK is for check-lock.sh.
  [[ -n "$RS_DRIVER_MIRROR" ]] || fail "mirror: RS_DRIVER_MIRROR is set but empty"
  [[ -d "$RS_DRIVER_MIRROR" ]] || fail "mirror: RS_DRIVER_MIRROR is not a directory: $RS_DRIVER_MIRROR"
  # The retained copies are keyed by the package's SHA-256, so they are read only when a recorded
  # lock names this version and platform. Otherwise the run downloads and says so, as
  # fetch-driver.sh does for a mirror that lacks the package: a caller that sets the variable for
  # every step can still verify a version that has no retained copies yet.
  if [[ -z "${RS_DRIVER_LOCK:-}" ]]; then
    why="no recorded lock is in RS_DRIVER_LOCK"
  elif ! lock="$(bash "$common_lib/../check-lock.sh" "$RS_DRIVER_LOCK")"; then
    fail "mirror: the recorded lock failed check-lock.sh"
  elif [[ "$(awk '$1 == "version" { print $2 }' <<<"$lock")" != "$version" ]]; then
    why="the recorded lock $RS_DRIVER_LOCK is not for $version"
  elif ! row="$(awk -v p=" $* " '$1 == "driver" && index(p, " " $2 " ") { print; exit }' <<<"$lock")" || [[ -z "$row" ]]; then
    why="the recorded lock $RS_DRIVER_LOCK has no ${names// / or } driver row"
  else
    read -r _ row_platform _ _ _ lock_url want_package_sha _ <<<"$row"
    [[ "$lock_url" == "$url" ]] ||
      fail "mirror: the recorded lock's $row_platform row is for $lock_url, not $url"
    mirror="$RS_DRIVER_MIRROR"
  fi
  [[ -z "$why" ]] ||
    echo "verify-release: $platform: RS_DRIVER_MIRROR is set but $why; downloading from ${url%/*}" >&2
}

get() {
  local msg
  if [[ -n "$mirror" ]]; then
    [[ -f "$mirror/$2" && ! -L "$mirror/$2" ]] ||
      fail "mirror: $mirror/$2 is missing or is not a regular file"
    # One byte past the bound is read at most, so a file that grows is caught as too large.
    head -c "$(($4 + 1))" -- "$mirror/$2" > "$3" || fail "mirror: cannot read $mirror/$2"
    (($(size_of "$3") <= $4)) || fail "mirror: $mirror/$2 is larger than $4 bytes"
  else
    msg="$(bash "$common_lib/download.sh" "$1" "$3" "$4" 2>&1)" || fail "download: $msg"
  fi
}

notice_rows() {
  local version="$1" odbc_dir="$2" work="$3" found="$4" notice_dir="${5:-}" take="${6:-}" notice source got want
  local name path bad="" msg packed var packaged_LICENSE="" packaged_NOTICE="" packaged_THIRD_PARTY_LICENSES=""
  # Each found member must be a notice's exact name at <notice-dir>, once; every other one is named,
  # before anything is downloaded. (No associative array: macOS's bash 3.2 has none.)
  while read -r name path; do
    [[ -n "$name" ]] || continue
    case " ${notices[*]} " in
    *" $name "*)
      var="packaged_$name"
      if [[ -n "$notice_dir" && "$path" == "$notice_dir/$name" && -z "${!var}" ]]; then
        printf -v "$var" '%s' "$path"
      else
        bad="${bad:+$bad, }$path"
      fi ;;
    *) bad="${bad:+$bad, }$path" ;;
    esac
  done <<<"$found"
  [[ -z "$bad" ]] ||
    fail "notices: the package holds $bad, named like a notice but not the one regular copy beside the driver; the release tag's copy ships"
  rows=()
  for notice in "${notices[@]}"; do
    mkdir "$work/notice-$notice"
    source="https://raw.githubusercontent.com/aws/amazon-redshift-odbc-driver/v${version%.*}/$notice"
    get "$source" "$want_package_sha-$notice" "$work/notice-$notice/$notice" 1048576
    [[ -f "$odbc_dir/$notice" ]] || fail "notices: the checked-in $notice is missing"
    got="$(sha256 "$work/notice-$notice/$notice")" || fail "notices: $got"
    want="$(sha256 "$odbc_dir/$notice")" || fail "notices: the checked-in $notice: $want"
    [[ "$got" == "$want" ]] || fail "notices: $notice from $source has SHA-256 $got; the checked-in copy has $want"
    # The package's own copy, if it has one, must be the release tag's: the tag's ships.
    var="packaged_$notice"
    if [[ -n "${!var}" ]]; then
      path="${!var}"
      mkdir "$work/packaged-$notice"
      msg="$("$take" "$path" "$work/packaged-$notice/$notice" 2>&1)" ||
        fail "notices: cannot take $path from the package: $msg"
      packed="$(sha256 "$work/packaged-$notice/$notice")" || fail "notices: $packed"
      [[ "$packed" == "$got" ]] ||
        fail "notices: the package's $path has SHA-256 $packed; $notice from $source has $got"
    fi
    rows+=("*  notice  $notice  $got  $source")
  done
}
