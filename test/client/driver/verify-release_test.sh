#!/usr/bin/env bash
# Tool tests for third_party/redshift_odbc/tools/verify-release.sh, the command, on the real
# release files of the lock under test: each Linux release the lock names passes, the signed bytes
# must be the version and architecture asked for, neither a changed pin nor the environment
# changes what is trusted, and the rows printed are the lock's. The lock's macOS package passes
# through the dispatch to lib/verify_macos.sh, whose rules verify-macos-*_test.sh test, and either
# macOS platform prints both macOS rows. Run by run-tool-tests.sh.
#
# The release files (package, signature, certificate, and the notices the lock takes from the
# release tag) come from the cache in release-cache.sh,
# filled from RS_DRIVER_MIRROR or by one download each, so the suite runs offline once the cache is
# warm. The command itself downloads through a stand-in curl that serves those files and records
# each call, or reads the retained copies when RS_DRIVER_MIRROR and RS_DRIVER_LOCK are set. The
# chain is always checked at the current time, so every case that verifies a real release skips
# itself once that release's signing certificate is outside its validity.
set -euo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=verify-test-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/verify-test-lib.sh"

base_lock="${RS_DRIVER_LOCK:-$odbc/driver.lock}"
[[ "$base_lock" == /* ]] || base_lock="$PWD/$base_lock"
lock="$(bash "$tools/check-lock.sh" "$base_lock")"
version="$(awk '$1 == "version" {print $2}' <<<"$lock")"
serve="$tmp/serve"
mkdir -p "$serve"
platforms=()
for platform in linux_amd64 linux_arm64; do
  row="$(awk -v p="$platform" '$1 == "driver" && $2 == p' <<<"$lock")"
  if [[ -z "$row" ]]; then echo "ok - $platform # SKIP the lock under test has no $platform row"; continue; fi
  read -r _ _ _ _ _ url pkg_sha _ <<<"$row"
  pkg="${url##*/}"
  cp "$(release_file "$pkg_sha" "$url" "$pkg_sha")" "$serve/$pkg"
  for f in signature.bin certificate.pem; do
    cp "$(release_file "$pkg_sha-$f" "${url%.rpm}-$f")" "$serve/${pkg%.rpm}-$f"
  done
  while read -r kind notice _ _ source; do
    [[ "$kind" == notice && "$source" == https://* ]] || continue
    cp "$(release_file "$pkg_sha-$notice" "$source")" "$serve/$notice"
  done <<<"$lock"
  platforms+=("$platform")
done
# The lock's rows for a platform, and the notice rows, with single spaces.
lock_rows() { awk -v p="$1" '{ sub(/#.*/, "") } $1 == p || $1 == "*" { $1 = $1; print }' "$base_lock"; }
printed_rows() { awk -v p="$1" '$1 == p || $1 == "*" { $1 = $1; print }' <<<"$out_text"; }
export FAKE_CURL_DIR="$serve"
arch_of() { case "$1" in linux_amd64) echo x86_64 ;; linux_arm64) echo aarch64 ;; esac; }
# valid_now <certificate>: prints "<verdict> <notBefore> <notAfter>", where the verdict is valid,
# outside or unreadable, so a date this suite cannot read is not taken for an expired certificate.
valid_now() {
  python3 -I -c 'import ssl, sys, time
try:
    start, end = (ssl.cert_time_to_seconds(l.split("=", 1)[1].strip())
                  for l in sys.stdin.read().splitlines())
except (ValueError, IndexError) as e:
    print("unreadable %s" % e); sys.exit(0)
print("%s %s %s" % ("valid" if start <= time.time() <= end else "outside",
                    time.strftime("%Y-%m-%d", time.gmtime(start)),
                    time.strftime("%Y-%m-%d", time.gmtime(end))))' < <(openssl x509 -noout -startdate -enddate -in "$1")
}

# verify <platform> [<tools-dir>]: runs the command with the stand-in curl first on PATH and any
# extra environment in env_extra; sets out_text and status. The caller's RS_DRIVER_MIRROR and
# RS_DRIVER_LOCK are dropped, so only a case that sets them in env_extra reads retained copies.
env_extra=()
verify() {
  rm -f "$FAKE_CURL_LOG" "$FAKE_CURL_LOG.args"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" ${env_extra[@]+"${env_extra[@]}"} \
    bash "${2:-$tools}/verify-release.sh" "$version" "$1" 2>&1)" && status=0 || status=$?
}
official="https://s3.amazonaws.com/redshift-downloads/drivers/odbc/$version"
# has_args <n> <flag> <value>: the n-th curl call had <flag> followed by <value>.
has_args() {
  awk -v n="$1" -v f="$2" -v v="$3" '$0 == "@@" { call++; next } call == n - 1 && prev == f && $0 == v { ok = 1 } { prev = $0 }
    END { exit !ok }' "$FAKE_CURL_LOG.args"
}

# Every case that verifies a real release needs its signing certificate to be valid now. A
# certificate whose dates this suite cannot read fails the suite: it is not an expired one.
signer_valid=1 why=""
for platform in ${platforms[@]+"${platforms[@]}"}; do
  validity="$(valid_now "$serve/AmazonRedshiftODBC-64-bit-$version.$(arch_of "$platform")-certificate.pem")"
  case "$validity" in
  valid*) ;;
  outside*) signer_valid=0; why="the $platform signing certificate is $validity" ;;
  *) fail "the $platform signing certificate's validity cannot be read: $validity"; signer_valid=0
     why="the $platform signing certificate is $validity" ;;
  esac
done
if ((signer_valid == 0)); then
  echo "ok - the real $version releases # SKIP $why, so the pipeline reuses the recorded lock instead of verifying"
  platforms=()
fi
notice_rows=()
for platform in ${platforms[@]+"${platforms[@]}"}; do
  arch="$(arch_of "$platform")"
  verify "$platform"
  expect "the unmodified $platform release passes" 0 "AmazonRedshiftODBC-64-bit-$version.$arch.rpm is signed by AWS's certificate, through the pinned intermediate"
  [[ "$out_text" == *"the pinned intermediate $pinned_name has DER SHA-256 $(der_sha256 "$odbc/signing/linux/$pinned_name")"* ]] ||
    fail "the $platform run did not print the pinned intermediate's DER SHA-256: $out_text"
  [[ "$out_text" == *"verify-release: $platform: the driver fits the Linux policy"* &&
    "$out_text" == *"verify-release: $platform: the driver's "*" scanned names are all classified"* ]] ||
    fail "the $platform release passed without the Linux policy or the environment step: $out_text"
  rows="$(printed_rows "$platform")"
  if [[ -n "$rows" && "$rows" == "$(lock_rows "$platform")" ]]; then
    pass "it prints the lock's $platform and notice rows"; else fail "$platform rows: $rows"; fi
  notice_rows+=("$(grep '^\* ' <<<"$rows" || true)")
done
# Notice rows apply to every platform, so every platform's run must print the same ones.
if ((${#notice_rows[@]} < 2)); then echo "ok - identical notice rows # SKIP fewer than two Linux platforms"
elif [[ -n "${notice_rows[0]}" && "${notice_rows[0]}" == "${notice_rows[1]}" ]]; then
  pass "both Linux platforms print the same notice rows"; else fail "notice rows differ: ${notice_rows[*]}"; fi
# The cases below run on the first Linux platform the lock names, P with architecture A; O and OA
# are the other one, when the lock names both.
if ((${#platforms[@]} == 0 && signer_valid == 1)); then fail "the lock under test names no Linux platform, so nothing was verified"; fi
P="${platforms[0]:-}" A="" O="${platforms[1]:-}" OA=""
[[ -z "$P" ]] || A="$(arch_of "$P")"
[[ -z "$O" ]] || OA="$(arch_of "$O")"
if [[ -n "$P" ]]; then
  verify $P
  p="$official/AmazonRedshiftODBC-64-bit-$version.$A"
  if [[ "$(head -3 "$FAKE_CURL_LOG")" == "$p.rpm"$'\n'"$p-signature.bin"$'\n'"$p-certificate.pem" &&
    "$(tail -n +4 "$FAKE_CURL_LOG")" == "$(awk '$1 == "notice" && $5 ~ /^https:\/\// { print $5 }' <<<"$lock")" ]] &&
    has_args 1 --max-filesize 268435456 && has_args 2 --max-filesize 65536 && has_args 3 --max-filesize 65536 &&
    has_args 1 --proto '=https' && has_args 2 --proto-redir '=https' && has_args 3 --max-redirs 0 &&
    calls="$(wc -l < "$FAKE_CURL_LOG")" && ((calls >= 3)) &&
    [[ "$(grep -c '^-q$' "$FAKE_CURL_LOG.args")" == "$calls" && "$(grep -c '^--path-as-is$' "$FAKE_CURL_LOG.args")" == "$calls" ]] &&
    ! grep -qE '^(-L|--location|-k|--insecure)$' "$FAKE_CURL_LOG.args"; then
    pass "it downloads the release files from the official path and the notices from the lock's sources: HTTPS only, no redirects, each size bounded"
  else fail "download calls: $(tr '\n' ' ' < "$FAKE_CURL_LOG.args")"; fi
  serve_copy() { local d="$tmp/$1"; mkdir -p "$d"; cp "$serve"/*.$A* "$d/"; echo "$d"; }

  # The signed bytes must be the version and architecture asked for.
  other="$tmp/other-version"; mkdir -p "$other"
  for f in "$serve"/AmazonRedshiftODBC-64-bit-"$version".$A*; do
    cp "$f" "$other/$(basename "$f" | sed "s/-$version\\./-9.9.9.0./")"
  done
  env_extra=(FAKE_CURL_DIR="$other"); version_saved="$version"; version=9.9.9.0
  verify $P; version="$version_saved"; env_extra=()
  expect "a signed release served under another version's path fails" 1 \
    "package: the signed package is AmazonRedshiftODBC-64-bit ${version%.*} ${version##*.} $A (name version release architecture), not the requested AmazonRedshiftODBC-64-bit 9.9.9 0 $A"
  if [[ -n "$O" ]]; then
    swapped="$tmp/swapped"; mkdir -p "$swapped"
    for f in "$serve"/AmazonRedshiftODBC-64-bit-"$version".$OA*; do cp "$f" "$swapped/$(basename "$f" | sed "s/\\.$OA/.$A/")"; done
    env_extra=(FAKE_CURL_DIR="$swapped"); verify $P; env_extra=()
    expect "the $OA release served as the $A one fails" 1 "package: the signed package is AmazonRedshiftODBC-64-bit ${version%.*} ${version##*.} $OA"
  fi

  # SYSTEM_CA_BUNDLE and CURL_CA_BUNDLE, and openssl's own variables, change nothing: the
  # release still passes, and a forged one that they would trust still fails. The configuration
  # is one openssl applies (checked first), so the case shows it is not read.
  mkdir -p "$tmp/certdir"; cp "$pki/root.pem" "$tmp/certdir/$(openssl x509 -hash -noout -in "$pki/root.pem").0"
  env_extra=(SYSTEM_CA_BUNDLE="$pki/root.pem" SSL_CERT_FILE="$pki/root.pem" SSL_CERT_DIR="$tmp/certdir"
    SSL_CERT_URI="file:$pki/root.pem" CURL_CA_BUNDLE="$pki/root.pem")
  verify $P
  expect "with SYSTEM_CA_BUNDLE and openssl's variables naming a test root, the release still passes" 0 "is signed by AWS's certificate"
  if hostile_case "with an OPENSSL_CONF that openssl applies, the release still passes"; then
    env_extra=(OPENSSL_CONF="$hostile_conf"); verify $P; env_extra=()
    expect "with an OPENSSL_CONF that openssl applies, the release still passes" 0 "is signed by AWS's certificate"
  fi
  env_extra=(SSL_CERT_FILE="$pki/root.pem" SSL_CERT_DIR="$tmp/certdir" SSL_CERT_URI="file:$pki/root.pem" SYSTEM_CA_BUNDLE="$pki/root.pem")
  forged="$(serve_copy forged)"
  pki_cert aws-by-test root "$aws_subject" codesign
  cp "$pki/aws-by-test.pem" "$forged/AmazonRedshiftODBC-64-bit-$version.$A-certificate.pem"
  pki_sign aws-by-test "$forged/AmazonRedshiftODBC-64-bit-$version.$A.rpm" "$forged/AmazonRedshiftODBC-64-bit-$version.$A-signature.bin"
  env_extra+=(FAKE_CURL_DIR="$forged")
  verify $P
  expect "a release signed under a test root that those variables name fails at the chain" 1 "verify-release: $P: chain: the certificate does not chain"
  env_extra=()

  # A wrong signature: another file's, served for the $A package.
  wrong="$(serve_copy wrong)"
  pki_sign leaf "$wrong/AmazonRedshiftODBC-64-bit-$version.$A.rpm" "$wrong/AmazonRedshiftODBC-64-bit-$version.$A-signature.bin"
  env_extra=(FAKE_CURL_DIR="$wrong"); verify $P; env_extra=()
  expect "another key's signature fails at the signature" 1 "verify-release: $P: signature: the detached signature does not verify"
  big="$(serve_copy big)"; head -c 70000 /dev/zero > "$big/AmazonRedshiftODBC-64-bit-$version.$A-signature.bin"
  env_extra=(FAKE_CURL_DIR="$big"); verify $P; env_extra=()
  expect "a signature over 64 KiB is stopped while it is written" 1 "download: downloading $official/AmazonRedshiftODBC-64-bit-$version.$A-signature.bin failed"

  # The pinned intermediate: the test PKI's in its place fails at the chain, because it is then the
  # only untrusted certificate given to openssl; the run also prints its DER SHA-256.
  t="$tmp/pin/third_party/redshift_odbc"; mkdir -p "$t" "$tmp/pin/src/transport"; cp -R "$odbc/." "$t/"
  cp "$repo/src/transport/odbc_driver.hpp" "$tmp/pin/src/transport/"
  cp "$pki/ca.pem" "$t/signing/linux/$pinned_name"
  verify $P "$t/tools"
  if [[ "$status" == 1 && "$out_text" == *"verify-release: $P: chain: the certificate does not chain"* &&
    "$out_text" == *"has DER SHA-256 $(der_sha256 "$pki/ca.pem")"* ]]; then
    pass "another certificate in signing/ fails at the chain, and its own fingerprint is printed"
  else fail "changed intermediate (exit $status; $out_text)"; fi
  # A signing/ file that is not one PEM certificate fails before any download.
  t="$tmp/bad-cert/third_party/redshift_odbc"; mkdir -p "$t" "$tmp/bad-cert/src/transport"; cp -R "$odbc/." "$t/"
  cp "$repo/src/transport/odbc_driver.hpp" "$tmp/bad-cert/src/transport/"
  printf 'not a certificate\n' > "$t/signing/linux/$pinned_name"
  env_extra=(FAKE_CURL=fail); verify $P "$t/tools"; env_extra=()
  if [[ "$status" == 1 && "$out_text" == *"certificate: the pinned intermediate: certificate: "*"must hold exactly one PEM certificate"* &&
    ! -e "$FAKE_CURL_LOG" ]]; then
    pass "a signing/ file that is not one PEM certificate fails before any download"
  else fail "unreadable intermediate (exit $status; $out_text)"; fi

  # The policy is the tree's own: without libkeyutils.so.1 in it, the real release fails, and no
  # lock row is printed.
  t="$tmp/policy/third_party/redshift_odbc"; mkdir -p "$t" "$tmp/policy/src/transport"; cp -R "$odbc/." "$t/"
  cp "$repo/src/transport/odbc_driver.hpp" "$tmp/policy/src/transport/"
  sed -i.orig '/^libkeyutils\.so\.1$/d' "$t/policy/linux/allowed-needed.txt"
  verify $P "$t/tools"
  expect "the release fails a policy without libkeyutils.so.1, naming it" 1 "verify-release: $P: needed: the driver needs libraries not in allowed-needed.txt: libkeyutils.so.1"
  if [[ -z "$(printed_rows $P)" ]]; then pass "a release that fails the policy prints no lock row"; else
    fail "rows printed after a policy failure: $(printed_rows $P)"; fi
  # The environment-name rule runs too: with PGHOST and SSL_ERROR_SYSCALL unclassified, the real
  # release fails, naming both, and prints no lock row.
  t="$tmp/env/third_party/redshift_odbc"; mkdir -p "$t" "$tmp/env/src/transport"; cp -R "$odbc/." "$t/"
  cp "$repo/src/transport/odbc_driver.hpp" "$tmp/env/src/transport/"
  sed -i.orig '/^PGHOST /d' "$t/policy/known-environment.txt"; sed -i.orig '/^SSL_ERROR_SYSCALL /d' "$t/policy/ignored-strings.txt"
  verify $P "$t/tools"
  if [[ "$status" == 1 && "$out_text" == *"verify-release: $P: environment: the driver names PGHOST,"* &&
    "$out_text" == *"verify-release: $P: environment: the driver names SSL_ERROR_SYSCALL,"* && -z "$(printed_rows $P)" ]]; then
    pass "the release fails on names the policy does not classify, naming each, and prints no row"
  else fail "environment step (exit $status; $out_text)"; fi
  t="$tmp/no-header/third_party/redshift_odbc"; mkdir -p "$t"; cp -R "$odbc/." "$t/"
  env_extra=(FAKE_CURL=fail); verify $P "$t/tools"; env_extra=()
  if [[ "$status" == 1 && "$out_text" == *"entry-points: the interface header "*" is missing or unreadable"* && ! -e "$FAKE_CURL_LOG" ]]; then
    pass "a tree without the interface header fails before any download"; else fail "missing header (exit $status; $out_text)"; fi

  # An openssl older than 1.1.0, and each missing tool, fail before any download, naming it.
  mkdir -p "$tmp/old-openssl"
  printf '#!/usr/bin/env bash\n[[ "$1" != version ]] || { echo "OpenSSL 1.0.2k-fips  26 Jan 2017"; exit 0; }\nexec %s "$@"\n' \
    "$(command -v openssl)" > "$tmp/old-openssl/openssl"; chmod +x "$tmp/old-openssl/openssl"
  rm -f "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/old-openssl:$tmp/bin:$PATH" FAKE_CURL=fail bash "$tools/verify-release.sh" "$version" $P 2>&1)" &&
    status=0 || status=$?
  if [[ "$status" == 1 && "$out_text" == *"openssl: verification needs OpenSSL 1.1.0 or later"* && ! -e "$FAKE_CURL_LOG" ]]; then
    pass "an openssl older than 1.1.0 fails before any download"; else fail "old openssl (exit $status; $out_text)"; fi
  tools_needed=(python3 readelf od curl)
  for missing in "${tools_needed[@]}"; do
    # Each case needs every other tool on the host (a macOS host has no readelf); without one, the
    # case skips, naming it, and the suite goes on to its macOS section.
    d="$tmp/without-$missing"; mkdir -p "$d"; absent=""
    for t in "${tools_needed[@]}"; do
      [[ "$t" == "$missing" || "$t" == curl ]] && continue
      if command -v "$t" > /dev/null; then ln -s "$(command -v "$t")" "$d/"; else absent="${absent:-$t}"; fi
    done
    if [[ -n "$absent" ]]; then
      n=$((n+1)); echo "ok $n - without $missing on PATH, the run fails before any download, naming it # SKIP $absent is not on this host"
      continue
    fi
    ln -s "$(command -v bash)" "$(command -v dirname)" "$d/"
    [[ "$missing" == curl ]] || ln -s "$tmp/bin/curl" "$d/"
    rm -f "$FAKE_CURL_LOG"
    out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$d" FAKE_CURL=fail "$(command -v bash)" "$tools/verify-release.sh" "$version" $P 2>&1)" &&
      status=0 || status=$?
    if [[ "$status" == 1 && "$out_text" == *"verify-release: $P: tools: $missing is required and was not found on PATH"* &&
      ! -e "$FAKE_CURL_LOG" ]]; then
      pass "without $missing on PATH, the run fails before any download, naming it"; else fail "without $missing (exit $status; $out_text)"; fi
  done

  # Only the trust store passed in is trusted: the real release, which chains to DigiCert's root in
  # the system store, fails at the chain against a store holding only the test root, so neither
  # openssl's default directory nor its default store is consulted (-no-CApath, -no-CAstore).
  real_cert="$serve/AmazonRedshiftODBC-64-bit-$version.$A-certificate.pem"
  signer_real() {
    out_text="$(bash "$tools/lib/linux_signer.sh" verify "$odbc/signing/linux/$pinned_name" \
      "$1" "$real_cert" "$serve/AmazonRedshiftODBC-64-bit-$version.$A-signature.bin" \
      "$serve/AmazonRedshiftODBC-64-bit-$version.$A.rpm" 2>&1)" && status=0 || status=$?
  }
  for b in /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem; do [[ -f "$b" ]] && break; done
  signer_real "$b"
  expect "the real release passes the signer rules against the system trust store" 0 ""
  signer_real "$pki/bundle.pem"
  expect "against a trust store without DigiCert's root, the real release fails at the chain" 1 "chain: the certificate does not chain"

  # The retained copies, read with the public URL unreachable: the run reads
  # $RS_DRIVER_MIRROR/<package-sha256> and its siblings, downloads nothing, and prints the recorded
  # lock's rows (the driver mirror).
  retained="$tmp/retained"; mkdir -p "$retained"
  read -r _ _ _ _ _ _ p_sha _ <<<"$(awk -v p="$P" '$1 == "driver" && $2 == p' <<<"$lock")"
  cp "$serve/AmazonRedshiftODBC-64-bit-$version.$A.rpm" "$retained/$p_sha"
  cp "$serve/AmazonRedshiftODBC-64-bit-$version.$A-signature.bin" "$retained/$p_sha-signature.bin"
  cp "$serve/AmazonRedshiftODBC-64-bit-$version.$A-certificate.pem" "$retained/$p_sha-certificate.pem"
  while read -r kind notice _ _ source; do
    [[ "$kind" == notice && "$source" == https://* ]] || continue
    cp "$serve/$notice" "$retained/$p_sha-$notice"
  done <<<"$lock"
  # Every platform, as a lock's regeneration covers every platform's rows.
  for platform in "${platforms[@]}"; do
    a="$(arch_of "$platform")"
    read -r _ _ _ _ _ _ sha _ <<<"$(awk -v p="$platform" '$1 == "driver" && $2 == p' <<<"$lock")"
    cp "$serve/AmazonRedshiftODBC-64-bit-$version.$a.rpm" "$retained/$sha"
    cp "$serve/AmazonRedshiftODBC-64-bit-$version.$a-signature.bin" "$retained/$sha-signature.bin"
    cp "$serve/AmazonRedshiftODBC-64-bit-$version.$a-certificate.pem" "$retained/$sha-certificate.pem"
    while read -r kind notice _ _ source; do
      [[ "$kind" == notice && "$source" == https://* ]] || continue
      cp "$serve/$notice" "$retained/$sha-$notice"
    done <<<"$lock"
    env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$base_lock"); verify "$platform"; env_extra=()
    if [[ "$status" == 0 && "$(printed_rows "$platform")" == "$(lock_rows "$platform")" && ! -e "$FAKE_CURL_LOG" ]] &&
      [[ "$out_text" == *"comes from the copy retained as $sha, not from $official"* ]]; then
      pass "with the retained copies the $platform run prints the recorded lock's rows and downloads nothing"
    else fail "retained copies, $platform (exit $status; $out_text)"; fi
  done
  # The rows of every platform, assembled into a lock, are the recorded lock: what a lock update
  # compares for a version whose lock was already released. The comparison is of check-lock.sh's
  # normalised output, since the printed rows are single-spaced and a lock's columns are padded.
  built="$tmp/rebuilt.lock"
  { awk '{ sub(/#.*/, "") } $1 == "format" || $1 == "version" || $1 == "status"' "$base_lock"
    for platform in "${platforms[@]}"; do
      env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$base_lock")
      out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" ${env_extra[@]+"${env_extra[@]}"} \
        bash "$tools/verify-release.sh" "$version" "$platform" 2>/dev/null)" || out_text="" # a failed run's rows are missing
      env_extra=()
      awk -v p="$platform" '$1 == p' <<<"$out_text"
      [[ "$platform" != "${platforms[0]}" ]] || grep '^\* ' <<<"$out_text" || true
    done; } > "$built"
  # macOS's rows join below; the Windows rows are verify-windows-msi_test.sh's real-MSI case's.
  grep -v '^windows_amd64 ' "$base_lock" > "$tmp/no-windows.lock"
  grep -v '^osx_' "$tmp/no-windows.lock" > "$tmp/linux-rows.lock"
  if [[ "$(bash "$tools/check-lock.sh" "$built")" == "$(bash "$tools/check-lock.sh" "$tmp/linux-rows.lock")" ]]; then
    pass "the rows of every Linux platform, assembled into a lock, are the lock under test's"
  else fail "rebuilt lock: $(bash "$tools/check-lock.sh" "$built" 2>&1)"; fi
  # With the retained copies, curl is not needed.
  d="$tmp/mirror-no-curl"; mkdir -p "$d"
  for c in bash env python3 readelf od objcopy awk sed grep sort comm tr cat cp head mv mkdir mktemp rm basename dirname openssl; do
    [[ ! -x "$(command -v "$c" 2>/dev/null)" ]] || ln -sf "$(command -v "$c")" "$d/"
  done
  out_text="$(env PATH="$d" RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$base_lock" \
    "$(command -v bash)" "$tools/verify-release.sh" "$version" $P 2>&1)" && status=0 || status=$?
  if [[ "$status" == 0 && "$(awk -v p="$P" '$1 == p || $1 == "*" { $1 = $1; print }' <<<"$out_text")" == "$(lock_rows $P)" ]]; then
    pass "with the retained copies the run needs no curl on PATH"
  else fail "mirror without curl (exit $status; $out_text)"; fi
  # Nor does a download need coreutils timeout: the same tools and the stand-in curl alone.
  d="$tmp/download-no-timeout"; mkdir -p "$d"; cp -P "$tmp/mirror-no-curl"/* "$d/"; ln -sf "$tmp/bin/curl" "$d/curl"
  rm -f -- "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$d" "$(command -v bash)" "$tools/verify-release.sh" "$version" $P 2>&1)" &&
    status=0 || status=$?
  if [[ "$status" == 0 && -e "$FAKE_CURL_LOG" &&
    "$(awk -v p="$P" '$1 == p || $1 == "*" { $1 = $1; print }' <<<"$out_text")" == "$(lock_rows $P)" ]]; then
    pass "a download needs no coreutils timeout on PATH"
  else fail "download without timeout (exit $status; $out_text)"; fi
  # A mirror the recorded lock does not go with is not an error: the run says so and downloads, so
  # a caller that sets the variable for every step can still verify a new version.
  # Here the stand-in curl serves the release files, so those runs pass.
  env_extra=(RS_DRIVER_MIRROR="$retained"); verify $P; env_extra=()
  if [[ "$status" == 0 && "$out_text" == *"RS_DRIVER_MIRROR is set but no recorded lock is in RS_DRIVER_LOCK; downloading"* ]] &&
    [[ -e "$FAKE_CURL_LOG" ]]; then
    pass "RS_DRIVER_MIRROR without RS_DRIVER_LOCK downloads, saying so"
  else fail "mirror without a lock (exit $status; $out_text)"; fi
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$tmp/no-such-dir" RS_DRIVER_LOCK="$base_lock"); verify $P; env_extra=()
  expect "an RS_DRIVER_MIRROR that is not a directory fails" 1 "mirror: RS_DRIVER_MIRROR is not a directory"
  short="$tmp/retained-short"; mkdir -p "$short"; cp "$retained/$p_sha" "$short/"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$short" RS_DRIVER_LOCK="$base_lock"); verify $P; env_extra=()
  expect "a retained signature that is missing fails, naming it" 1 "mirror: $short/$p_sha-signature.bin is missing"
  tampered="$tmp/retained-tampered"; mkdir -p "$tampered"; cp "$retained"/* "$tampered/"
  printf 'tampered\n' >> "$tampered/$p_sha"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$tampered" RS_DRIVER_LOCK="$base_lock"); verify $P; env_extra=()
  expect "a tampered retained package fails against the recorded lock's hash" 1 "has SHA-256"
  [[ "$out_text" == *"not the SHA-256 the recorded lock names"* ]] ||
    fail "the tampered-package message does not name the recorded lock: $out_text"
  other_lock="$tmp/other-version.lock"; sed "s/$version/9.9.9.0/g" "$base_lock" > "$other_lock"
  env_extra=(RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$other_lock"); verify $P; env_extra=()
  if [[ "$status" == 0 && "$out_text" == *"is not for $version; downloading"* && -e "$FAKE_CURL_LOG" ]]; then
    pass "a recorded lock for another version downloads, saying so"
  else fail "a lock for another version (exit $status; $out_text)"; fi
  no_row_lock="$tmp/no-row.lock"; grep -v "^$P " "$base_lock" > "$no_row_lock"
  env_extra=(RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$no_row_lock"); verify $P; env_extra=()
  if [[ "$status" == 0 && "$out_text" == *"has no $P driver row; downloading"* && -e "$FAKE_CURL_LOG" ]]; then
    pass "a recorded lock without this platform's row downloads, saying so"
  else fail "a lock without the row (exit $status; $out_text)"; fi
  # An empty RS_DRIVER_MIRROR is an error, not a silent download.
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="" RS_DRIVER_LOCK="$base_lock"); verify $P; env_extra=()
  expect "an empty RS_DRIVER_MIRROR fails rather than downloading" 1 "mirror: RS_DRIVER_MIRROR is set but empty"
  bad_lock="$tmp/bad-sha.lock"; sed "s/  $p_sha  /  ${p_sha%??}  /" "$base_lock" > "$bad_lock"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$bad_lock"); verify $P; env_extra=()
  expect "a recorded lock the checker rejects fails, naming that" 1 "mirror: the recorded lock failed check-lock.sh"
  if [[ -n "$O" ]]; then
    read -r _ _ _ _ _ _ o_sha _ <<<"$(awk -v p="$O" '$1 == "driver" && $2 == p' <<<"$lock")"
    url_lock="$tmp/swapped-url.lock"
    sed "s|AmazonRedshiftODBC-64-bit-$version.$A.rpm|AmazonRedshiftODBC-64-bit-$version.$OA.rpm|" "$base_lock" > "$url_lock"
    env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$retained" RS_DRIVER_LOCK="$url_lock"); verify $P; env_extra=()
    expect "a recorded lock whose row names another platform's package fails" 1 "mirror: the recorded lock's $P row is for"
  fi
  # The retained files must be regular files of at most their bound: a link could point anywhere,
  # and the signature, certificate and notices are not hash-checked against the lock before use.
  linked="$tmp/retained-linked"; mkdir -p "$linked"; cp "$retained"/* "$linked/"
  rm "$linked/$p_sha-signature.bin"; ln -s "$retained/$p_sha-signature.bin" "$linked/$p_sha-signature.bin"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$linked" RS_DRIVER_LOCK="$base_lock"); verify $P; env_extra=()
  expect "a retained file that is a symbolic link fails" 1 "mirror: $linked/$p_sha-signature.bin is missing or is not a regular file"
  big_retained="$tmp/retained-big"; mkdir -p "$big_retained"; cp "$retained"/* "$big_retained/"
  head -c 70000 /dev/zero > "$big_retained/$p_sha-signature.bin"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$big_retained" RS_DRIVER_LOCK="$base_lock"); verify $P; env_extra=()
  expect "a retained file over its bound fails" 1 "is larger than 65536 bytes"

  # The lock rows are the only output on stdout; progress goes to stderr, where a caller that reads
  # only stdout does not see it.
  rm -f "$FAKE_CURL_LOG"
  stdout="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" bash "$tools/verify-release.sh" "$version" $P 2>"$tmp/stderr")" &&
    status=0 || status=$?
  if [[ "$status" == 0 && "$(awk '{ $1 = $1; print }' <<<"$stdout")" == "$(lock_rows $P)" ]] &&
    ! grep -qv '^verify-release: ' "$tmp/stderr"; then
    pass "stdout holds exactly the lock rows, and every progress line is on stderr"; else
    fail "stdout/stderr split (exit $status; stdout: $stdout; stderr: $(cat "$tmp/stderr"))"; fi

  env_extra=(FAKE_CURL=302); verify $P; env_extra=()
  expect "a redirect is a failure, not followed" 1 "download: $official/AmazonRedshiftODBC-64-bit-$version.$A.rpm answered HTTP 302"
  empty="$tmp/empty"; mkdir -p "$empty"
  env_extra=(FAKE_CURL_DIR="$empty"); verify $P; env_extra=()
  expect "a missing release file fails the download" 1 "download: downloading $official/AmazonRedshiftODBC-64-bit-$version.$A.rpm failed"
fi

# macOS, through the dispatch to lib/verify_macos.sh: the lock's universal package, from the
# release cache, and the notices the lock takes from the release tag pass, and either macOS
# platform's run prints both macOS rows, equal to the lock's, and the same notice rows as Linux;
# with the retained copies it downloads nothing. The chain is checked at the current time, so these
# cases skip once the package's signing certificate is outside its validity.
mac_row="$(awk '$1 == "driver" && ($2 == "osx_amd64" || $2 == "osx_arm64") { print; exit }' <<<"$lock")"
mac_valid=""
if [[ -z "$mac_row" ]]; then echo "ok - macOS # SKIP the lock under test has no macOS row"
elif sha1_refused; then echo "ok - macOS # SKIP $no_sha1"
else
  read -r _ _ _ _ _ mac_url mac_pkg_sha _ <<<"$mac_row"
  cp "$(release_file "$mac_pkg_sha" "$mac_url" "$mac_pkg_sha")" "$serve/${mac_url##*/}"
  while read -r kind notice _ _ source; do
    [[ "$kind" == notice && "$source" == https://* && ! -f "$serve/$notice" ]] || continue
    cp "$(release_file "$mac_pkg_sha-$notice" "$source")" "$serve/$notice"
  done <<<"$lock"
  mkdir "$tmp/mac-parts"
  python3 -I "$tools/lib/xar.py" signed "$serve/${mac_url##*/}" "$tmp/mac-parts" sha1 >/dev/null
  openssl x509 -inform DER -in "$tmp/mac-parts/signer.der" -out "$tmp/mac-signer.pem"
  validity="$(valid_now "$tmp/mac-signer.pem")"
  case "$validity" in
  valid*) mac_valid=1 ;;
  outside*) echo "ok - the real $version macOS release # SKIP its signing certificate is $validity, so the pipeline reuses the recorded lock instead of verifying" ;;
  *) fail "the macOS signing certificate's validity cannot be read: $validity" ;;
  esac
fi
mac_rows() { awk '$1 == "osx_amd64" || $1 == "osx_arm64" || $1 == "*" { $1 = $1; print }' <<<"$1"; }
lock_mac_rows="$(awk '{ sub(/#.*/, "") } $1 == "osx_amd64" || $1 == "osx_arm64" || $1 == "*" { $1 = $1; print }' "$base_lock")"
if [[ -n "$mac_valid" ]]; then
  first=""
  for platform in osx_amd64 osx_arm64; do
    verify "$platform"
    if [[ "$status" == 0 && "$(mac_rows "$out_text")" == "$lock_mac_rows" &&
      "$(head -1 "$FAKE_CURL_LOG")" == "$official/${mac_url##*/}" &&
      "$out_text" == *"verify-release: $platform: the library fits the macOS policy"* &&
      "$out_text" == *"verify-release: $platform: x86_64: the driver's "*"verify-release: $platform: arm64: the driver's "* ]]; then
      pass "the unmodified macOS release passes every rule through $platform and prints both macOS rows and the notice rows of the lock"
    else fail "the unmodified macOS release through $platform (exit $status; $out_text)"; fi
    [[ -z "$first" || "$first" == "$(mac_rows "$out_text")" ]] ||
      fail "osx_amd64 and osx_arm64 print different rows: $first / $(mac_rows "$out_text")"
    first="$(mac_rows "$out_text")"
  done
  if ((${#notice_rows[@]} > 0)); then
    if [[ "$(grep '^\* ' <<<"$first" || true)" == "${notice_rows[0]}" ]]; then
      pass "macOS prints the same notice rows as Linux"; else fail "macOS notice rows: $first"; fi
  fi
  rm -f "$FAKE_CURL_LOG"
  stdout="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" bash "$tools/verify-release.sh" "$version" osx_arm64 2>"$tmp/stderr")" &&
    status=0 || status=$?
  if [[ "$status" == 0 && "$(awk '{ $1 = $1; print }' <<<"$stdout")" == "$lock_mac_rows" ]] &&
    ! grep -qv '^verify-release: ' "$tmp/stderr"; then
    pass "on macOS too, stdout holds exactly the lock rows, and every progress line is on stderr"; else
    fail "macOS stdout/stderr split (exit $status; stdout: $stdout; stderr: $(cat "$tmp/stderr"))"; fi
  # The retained copies: the package and the notices by the package's SHA-256, with no detached
  # signature or certificate, and nothing downloaded.
  mac_retained="$tmp/mac-retained"; mkdir -p "$mac_retained"
  cp "$serve/${mac_url##*/}" "$mac_retained/$mac_pkg_sha"
  while read -r kind notice _ _ source; do
    [[ "$kind" == notice ]] && cp "$serve/$notice" "$mac_retained/$mac_pkg_sha-$notice"
  done <<<"$lock"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$mac_retained" RS_DRIVER_LOCK="$base_lock"); verify osx_amd64; env_extra=()
  if [[ "$status" == 0 && "$(mac_rows "$out_text")" == "$lock_mac_rows" && ! -e "$FAKE_CURL_LOG" ]] &&
    [[ "$out_text" == *"comes from the copy retained as $mac_pkg_sha, not from $official"* ]]; then
    pass "with the retained copies the macOS run prints the recorded lock's rows and downloads nothing"
  else fail "retained copies, macOS (exit $status; $out_text)"; fi
  # With the retained copies, curl is not needed on macOS either; nor does a macOS download need
  # coreutils timeout, which a fresh macOS host has not got.
  d="$tmp/mac-no-curl"; mkdir -p "$d"
  for c in bash env python3 awk sed grep tr cat cp head mv mkdir mktemp rm dirname openssl; do
    [[ ! -x "$(command -v "$c" 2>/dev/null)" ]] || ln -sf "$(command -v "$c")" "$d/"
  done
  out_text="$(env PATH="$d" RS_DRIVER_MIRROR="$mac_retained" RS_DRIVER_LOCK="$base_lock" \
    "$(command -v bash)" "$tools/verify-release.sh" "$version" osx_arm64 2>&1)" && status=0 || status=$?
  if [[ "$status" == 0 && "$(mac_rows "$out_text")" == "$lock_mac_rows" ]]; then
    pass "with the retained copies the macOS run needs no curl on PATH"
  else fail "macOS mirror without curl (exit $status; $out_text)"; fi
  mkdir -p "$tmp/mac-download-no-timeout"; cp -P "$d"/* "$tmp/mac-download-no-timeout/"
  ln -sf "$tmp/bin/curl" "$tmp/mac-download-no-timeout/curl"
  rm -f -- "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/mac-download-no-timeout" \
    "$(command -v bash)" "$tools/verify-release.sh" "$version" osx_arm64 2>&1)" && status=0 || status=$?
  if [[ "$status" == 0 && "$(mac_rows "$out_text")" == "$lock_mac_rows" && -e "$FAKE_CURL_LOG" ]]; then
    pass "a macOS download needs no coreutils timeout on PATH"
  else fail "macOS download without timeout (exit $status; $out_text)"; fi
  tampered="$tmp/mac-tampered"; mkdir -p "$tampered"; cp "$mac_retained"/* "$tampered/"
  printf 'tampered\n' >> "$tampered/$mac_pkg_sha"
  env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$tampered" RS_DRIVER_LOCK="$base_lock"); verify osx_arm64; env_extra=()
  expect "a tampered retained macOS package fails against the recorded lock's hash" 1 "not the SHA-256 the recorded lock names"
  # The rows of every platform, Linux and macOS, assembled into a lock, are the lock under test: what
  # a lock update compares for an already-released version, with the macOS rows from one run.
  if ((${#platforms[@]} > 0)) && [[ -s "${built:-}" ]]; then
    env_extra=(FAKE_CURL=fail RS_DRIVER_MIRROR="$mac_retained" RS_DRIVER_LOCK="$base_lock"); verify osx_arm64; env_extra=()
    awk '$1 == "osx_amd64" || $1 == "osx_arm64"' <<<"$out_text" >> "$built"
    want="$(bash "$tools/check-lock.sh" "$tmp/no-windows.lock")"
    if [[ "$(bash "$tools/check-lock.sh" "$built")" == "$want" ]]; then
      pass "the Linux and macOS rows, assembled into a lock, are the lock under test"
    else fail "rebuilt lock with macOS: $(bash "$tools/check-lock.sh" "$built" 2>&1)"; fi
  fi
fi

# Arguments, all refused before any download.
for v in latest 2.03.0.0 2.3.0 2.3.0.0.0 "" 02.3.0.0; do
  env_extra=(FAKE_CURL=fail); version_saved="$version"; version="$v"; verify linux_amd64; version="$version_saved"; env_extra=()
  if [[ "$status" == 1 && "$out_text" == *"version: check-lock: '$v': version '$v' is not four dot-separated numbers"* &&
    ! -e "$FAKE_CURL_LOG" ]]; then
    pass "version '$v' is refused before any download"; else fail "version '$v' (exit $status; $out_text)"; fi
done
# refused <name> <expected-exit> <substring> <argument>...: the command with the stand-in curl first
# on PATH, failing loudly if called (FAKE_CURL=fail), so no case here can reach the real curl;
# nothing may have been fetched.
refused() {
  local name="$1" want="$2" needle="$3"; shift 3
  rm -f "$FAKE_CURL_LOG"
  out_text="$(env -u RS_DRIVER_MIRROR -u RS_DRIVER_LOCK PATH="$tmp/bin:$PATH" FAKE_CURL=fail bash "$tools/verify-release.sh" "$@" 2>&1)" && status=0 || status=$?
  if [[ -e "$FAKE_CURL_LOG" ]]; then fail "$name (it fetched: $(tr '\n' ' ' < "$FAKE_CURL_LOG"))"; else expect "$name" "$want" "$needle"; fi
}
refused "an unknown platform fails" 1 "unknown platform windows_arm64" "$version" windows_arm64
for p in osx_amd64 osx_arm64; do
  refused "$p goes to the macOS verification, which refuses a bad version before any download" 1 \
    "verify-release: $p: version: check-lock: 'latest': version 'latest' is not four dot-separated numbers" latest "$p"
  refused "$p with a third argument is a usage error" 2 "usage: verify-release.sh <version> <platform>" "$version" "$p" extra
done
refused "a missing argument is a usage error" 2 "usage: verify-release.sh <version> <platform>" "$version"
refused "a third argument is a usage error" 2 "usage: verify-release.sh <version> <platform>" "$version" linux_amd64 extra
finish
