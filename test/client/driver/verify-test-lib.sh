# Helpers for the verification suites (verify-*_test.sh), sourced, never run on their own. They
# build every test input at test time: a test PKI made with openssl, test-signed release files,
# and a stand-in curl that serves files from a directory. Each suite sets tmp first. The macOS
# suites source macos-test-lib.sh, which builds on this file.
# shellcheck shell=bash

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=release-cache.sh
source "$repo/test/client/driver/release-cache.sh" # sha, require_tools, release_file
require_tools python3 openssl
odbc="$repo/third_party/redshift_odbc"
tools="$odbc/tools"
pinned_name="DigiCertTrustedG4CodeSigningRSA4096SHA3842021CA1.pem"
# The signer's extended-validation subject, as the real certificate records it and as the subject
# rule requires it (third_party/redshift_odbc/tools/lib/x509.py); aws_subject_with replaces one
# attribute's value, for a look-alike.
aws_subject="/jurisdictionC=US/jurisdictionST=Delaware/serialNumber=4152954/C=US/ST=Washington/L=Seattle/O=Amazon Web Services, Inc./OU=AWS Redshift/CN=Amazon Web Services, Inc."
aws_subject_with() { printf '%s' "$aws_subject" | sed "s|/$1=[^/]*|/$1=$2|"; }

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
finish() { echo "# $n cases, $((n - failed)) passed"; exit $(( failed > 0 )); }
# expect <name> <expected-exit> <substring>: checks $status and $out_text from the last run. A
# failure (exit 1) must also be the run's last line, so a rule whose failure did not stop the run
# fails the case even when a later step fails too.
expect() {
  if [[ "$status" == "$2" && "$out_text" == *"$3"* ]] &&
    { [[ "$2" != 1 ]] || [[ "${out_text##*$'\n'}" == *"$3"* ]]; }; then pass "$1"; else
    fail "$1 (exit $status, wanted $2; output: $out_text)"; fi
}

# The test PKI, in $tmp/pki: root (the test trust store), a second root, ca (the intermediate
# the test trees pin, issued by root) and leaves made with pki_cert. EC keys keep it fast.
pki="$tmp/pki"
mkdir -p "$pki"
cat > "$pki/openssl.cnf" <<'EOF'
[req]
distinguished_name = dn
[dn]
[ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[root]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[codesign]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[serverauth]
basicConstraints = critical,CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid:always
[noeku]
basicConstraints = critical,CA:FALSE
authorityKeyIdentifier = keyid:always
EOF
pki_key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$pki/$1.key" 2>/dev/null; }
# pki_self <name> <subject> <section>: a self-signed certificate.
pki_self() {
  pki_key "$1"
  openssl req -new -x509 -key "$pki/$1.key" -subj "$2" -days 2 -sha256 -config "$pki/openssl.cnf" \
    -extensions "$3" -out "$pki/$1.pem" 2>/dev/null
}
# pki_cert <name> <issuer> <subject> <section>: a certificate issued by <issuer>, for $pki/<name>.key,
# made as an EC key if it does not exist yet.
pki_cert() {
  local key="$1"
  [[ -f "$pki/$key.key" ]] || pki_key "$key"
  openssl req -new -key "$pki/$key.key" -subj "$3" -config "$pki/openssl.cnf" -out "$pki/$1.csr" 2>/dev/null
  openssl x509 -req -in "$pki/$1.csr" -CA "$pki/$2.pem" -CAkey "$pki/$2.key" -days 2 -sha256 \
    -set_serial "0x$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')" -extfile "$pki/openssl.cnf" \
    -extensions "$4" -out "$pki/$1.pem" 2>/dev/null
}
pki_self root "/CN=Test Root" root
pki_self root2 "/CN=Second Test Root" root
pki_cert ca root "/C=US/O=Test CA/CN=Test Code Signing CA" ca
pki_cert leaf ca "$aws_subject" codesign
cp "$pki/root.pem" "$pki/bundle.pem"
# pki_sign <leaf> <file> <signature>: a detached SHA-256 signature, as AWS publishes them.
pki_sign() { openssl dgst -sha256 -sign "$pki/$1.key" -out "$3" "$2"; }

# test_tree <name> [<intermediate>]: a copy of third_party/redshift_odbc whose signing/ holds the
# test intermediate (default: ca) as the pinned certificate. Prints the copy's directory.
test_tree() {
  local d="$tmp/$1/third_party/redshift_odbc" ca="${2:-ca}"
  mkdir -p "$d"; cp -R "$odbc/." "$d/"
  cp "$pki/$ca.pem" "$d/signing/linux/$pinned_name"
  echo "$d"
}
# der_sha256 <cert.pem>: the SHA-256 of a certificate's DER encoding, computed here with openssl
# rather than with the tool under test.
der_sha256() {
  openssl x509 -in "$1" -outform DER |
    python3 -I -c 'import hashlib,sys; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())'
}

# A stand-in curl, first on PATH in the runs below: FAKE_CURL=fail exits 99 when called; otherwise
# it serves $FAKE_CURL_DIR/<last segment of the URL> with 200, or answers 404, or FAKE_CURL=<code>
# answers <code>. Every URL asked for is appended to $FAKE_CURL_LOG, every call's arguments, one
# per line and then a line @@, to $FAKE_CURL_LOG.args, and SSL_CERT_FILE as the call saw it
# (unset, or its value) to $FAKE_CURL_LOG.env. A run starts by removing $FAKE_CURL_LOG, and its
# first call then starts the argument and environment logs afresh, so all three hold one run's
# calls: the stand-in runs under download.sh's ulimit -f, which bounds every file it writes, so a
# log kept across runs would grow into it (64 KiB for a signature) and fail a later download.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/curl" <<'EOF'
#!/usr/bin/env bash
out="" format="" url="${*: -1}"
[[ -e "$FAKE_CURL_LOG" ]] || { : > "$FAKE_CURL_LOG.args"; : > "$FAKE_CURL_LOG.env"; }
echo "$url" >> "$FAKE_CURL_LOG"
printf '%s\n' "$@" @@ >> "$FAKE_CURL_LOG.args"
echo "SSL_CERT_FILE=${SSL_CERT_FILE-unset}" >> "$FAKE_CURL_LOG.env"
[[ "${FAKE_CURL:-}" != fail ]] || exit 99
while (($#)); do case "$1" in --output) out="$2" ;; --write-out) format="$2" ;; esac; shift; done
[[ -n "$out" && "$format" == '%{http_code}' ]] || exit 98
if [[ -n "${FAKE_CURL:-}" ]]; then printf '%s' "$FAKE_CURL"; exit 0; fi
file="$FAKE_CURL_DIR/${url##*/}"
[[ -f "$file" ]] || { printf 404; exit 22; }
# cat, not cp, which on macOS may copy without write(2) (an APFS clone), past download.sh's ulimit -f.
cat -- "$file" > "$out" || exit 23
printf 200
EOF
chmod +x "$tmp/bin/curl"
export FAKE_CURL_LOG="$tmp/curl.log"

# A stand-in openssl, in $tmp/openssl-bin, which runs the real one and appends every call's
# arguments, one per line and then a line @@, to $OPENSSL_LOG; with OPENSSL_FAIL=<subcommand> it
# instead fails that subcommand, leaving the others alone. It lets a case see the options the
# chain check passes on any host, and reach the branches a working openssl never takes.
mkdir -p "$tmp/openssl-bin"
cat > "$tmp/openssl-bin/openssl" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" @@ >> "\$OPENSSL_LOG"
if [[ -n "\${OPENSSL_FAIL:-}" && "\$1" == "\$OPENSSL_FAIL" ]]; then
  echo "stand-in openssl: refusing \$1" >&2; exit 1
fi
exec $(command -v openssl) "\$@"
EOF
chmod +x "$tmp/openssl-bin/openssl"
export OPENSSL_LOG="$tmp/openssl.log"
# openssl_args <subcommand> <flag>: the recorded call of that subcommand had that flag.
openssl_args() {
  awk -v cmd="$1" -v f="$2" '
    $0 == "@@" { if (first == cmd && found) ok = 1; first = ""; found = 0; next }
    first == "" { first = $0 }
    $0 == f { found = 1 }
    END { exit !ok }' "$OPENSSL_LOG"
}

# An openssl configuration that openssl applies and that breaks it: on OpenSSL 3 it asks for
# FIPS-only algorithms, which the default provider does not offer, so openssl dgst fails. A case
# that sets OPENSSL_CONF to it, and still passes, shows the tools do not read it. OpenSSL 1.1
# ignores a configuration it cannot apply, and no setting of it breaks dgst or verify, so there
# hostile_conf is empty and such a case is skipped (hostile_case prints the skip).
hostile_conf=""
if [[ "$(openssl version)" != "OpenSSL 1."* ]]; then
  hostile_conf="$tmp/hostile-openssl.cnf"
  printf '%s\n' "openssl_conf = init" "[init]" "alg_section = algs" "[algs]" "default_properties = fips=yes" > "$hostile_conf"
  # Checked once: the configuration must change openssl's outcome, or the cases would prove nothing.
  if OPENSSL_CONF="$hostile_conf" openssl dgst -sha256 /dev/null >/dev/null 2>&1; then
    fail "the hostile openssl configuration does not change openssl's outcome"; hostile_conf=""
  fi
fi
# hostile_case <name>: prints a skip and returns 1 when there is no hostile configuration.
hostile_case() {
  [[ -n "$hostile_conf" ]] && return 0
  n=$((n+1)); echo "ok $n - $1 # SKIP $(openssl version | cut -d' ' -f1-2) ignores a configuration it cannot apply"; return 1
}

# sha1_refused: succeeds when openssl on PATH refuses a SHA-1 RSA signature (pkeyutl -sign with
# digest:sha1, as make_xar.py signs) or a SHA-1 CMS signature (cms -sign -md sha1, then cms
# -verify), while the same three steps pass with SHA-256. 2.3.0.0's macOS package holds one of each,
# and a host crypto policy can refuse them (RHEL 9's DEFAULT does; Amazon Linux 2023's does not).
# The cases that sign or verify a macOS package then skip with no_sha1, which ends with openssl's
# own message: the tools leave the host's policy alone, and verification belongs on a host where
# SHA-1 is allowed. A failure SHA-256 shares (no key, no cms command, a cms -verify that always
# fails, a full disk) is not a refusal, so the suite runs and fails.
no_sha1="openssl on this host refuses SHA-1 signatures (its crypto policy), which 2.3.0.0's macOS package uses; run where the policy allows them"
sign_probe() { # sign_probe <dir> <digest> <bytes>: the two signing steps and the CMS
  # verification; the failing step's first line on stderr
  printf "%0${3}d" 0 > "$1/m"
  { openssl pkeyutl -sign -inkey "$1/k.pem" -pkeyopt rsa_padding_mode:pkcs1 -pkeyopt "digest:$2" -in "$1/m" -out "$1/s" &&
    openssl cms -sign -binary -md "$2" -signer "$1/c.pem" -inkey "$1/k.pem" -in "$1/m" -outform DER -out "$1/c.der" &&
    openssl cms -verify -binary -inform DER -in "$1/c.der" -content "$1/m" -noverify -nointern -certfile "$1/c.pem" -out /dev/null
  } > /dev/null 2> "$1/err" || { head -n 1 "$1/err" >&2; return 1; }
}
sha1_refused() {
  local d status=1 err
  d="$(mktemp -d)"
  if openssl req -x509 -newkey rsa:2048 -nodes -keyout "$d/k.pem" -out "$d/c.pem" -subj /CN=sha1-probe -days 1 > /dev/null 2>&1 &&
    sign_probe "$d" sha256 32 2> /dev/null && ! err="$(sign_probe "$d" sha1 20 2>&1)"; then
    status=0; no_sha1="${no_sha1%% (openssl: *} (openssl: $err)"
  fi
  rm -rf -- "${d:?}"
  return "$status"
}
