# The cache of the locked release files for the tool tests, sourced, never run on its own. A
# package's release files are the package itself, its detached signature and certificate, and any
# notice its lock takes from the release tag; they sit in build/driver-package-cache/packages/ as
# <package-sha256>, <package-sha256>-signature.bin, <package-sha256>-certificate.pem and
# <package-sha256>-<notice>. A missing one is copied from RS_DRIVER_MIRROR when it holds a file of
# that name (retained copies, laid out the same way), else, for a package, from the
# copy fetch-driver.sh kept in build/driver-package-cache/<platform>/packages/ (fetch-driver_test.sh
# fills it first), and otherwise downloaded once by lib/download.sh, so the suites run offline once
# the cache is warm or with a mirror, and a package is never downloaded twice. Nothing else is
# fetched.
# shellcheck shell=bash

release_cache_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
release_cache_dir="$release_cache_root/build/driver-package-cache/packages"
mkdir -p "$release_cache_dir"
# require_tools <tool>...: each must be on PATH, or the suite fails here, naming it, rather than at
# the first command that needs it with no message.
require_tools() {
  local t
  for t in "$@"; do
    command -v "$t" >/dev/null || { echo "not ok 1 - $t is required by this suite and was not found on PATH"; exit 1; }
  done
}
# sha <file>: the file's SHA-256, the one helper the tool suites that source this file share.
sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
# locked_drivers <lock> <out-dir>: takes each Linux platform's driver out of its locked package,
# from the cache, into <out-dir>/<platform>.so, and prints one "<platform> <path>" line per row.
# The suites that read the real drivers share it.
locked_drivers() {
  local lock out="$2" platform url pkg_sha path tools
  tools="$release_cache_root/third_party/redshift_odbc/tools"
  lock="$(bash "$tools/check-lock.sh" "$1")" # a lock the checker rejects fails the suite here
  while read -r _ platform _ _ _ url pkg_sha path; do
    python3 -I "$tools/lib/rpm.py" --payload "$(release_file "$pkg_sha" "$url" "$pkg_sha")" |
      python3 -I "$tools/lib/cpio.py" "$path" "$out/$platform.so"
    echo "$platform $out/$platform.so"
  done < <(awk '$1 == "driver" && $2 ~ /^linux_/' <<<"$lock")
}
# release_file <name> <url> [<sha256>]: prints the cached file's path, filling the cache first;
# with <sha256>, the file must have that hash. Fails (status 1) when it cannot.
release_file() {
  local name="$1" url="$2" want="${3:-}" file="$release_cache_dir/$1" part src c
  if [[ ! -f "$file" ]] || { [[ -n "$want" ]] && [[ "$(sha "$file")" != "$want" ]]; }; then
    part="$(mktemp "$release_cache_dir/.part.XXXXXX")"
    src=""
    if [[ -n "${RS_DRIVER_MIRROR:-}" && -f "$RS_DRIVER_MIRROR/$name" ]]; then
      src="$RS_DRIVER_MIRROR/$name"
    elif [[ -n "$want" ]]; then
      # fetch-driver.sh's copy of a package, used only when it has the package's SHA-256
      for c in "$release_cache_dir"/../*/packages/"$name"; do
        if [[ -f "$c" && "$(sha "$c")" == "$want" ]]; then src="$c"; break; fi
      done
    fi
    if [[ -n "$src" ]]; then
      # Checked like the download: a partial copy must not become a cached file every later run
      # reuses. The callers run release_file in a command substitution, where set -e does not
      # apply on bash 4.2, so every step says so itself.
      cp -- "$src" "$part" || { echo "# copying $src into $release_cache_dir failed" >&2; rm -f -- "$part"; return 1; }
    else
      src="$url"
      echo "# downloading $url once into $release_cache_dir" >&2
      bash "$release_cache_root/third_party/redshift_odbc/tools/lib/download.sh" "$url" "$part" 268435456 >&2 ||
        { rm -f -- "$part"; return 1; }
    fi
    if [[ -n "$want" && "$(sha "$part")" != "$want" ]]; then
      echo "# $name from $src does not have SHA-256 $want" >&2; rm -f -- "$part"; return 1
    fi
    mv -f -- "$part" "$file" || { echo "# moving $part to $file failed" >&2; rm -f -- "$part"; return 1; }
  fi
  echo "$file"
}
