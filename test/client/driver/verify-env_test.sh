#!/usr/bin/env bash
# Tool tests for the environment-name rule, third_party/redshift_odbc/tools/lib/env_names.py: the
# checked-in policy classifies every name both real drivers hold; copies of a driver with one more
# name, and policies with a row removed, added or malformed, each fail, naming the string. Run by
# run-tool-tests.sh.
#
# The real drivers come from the packages of the lock under test in the release cache
# (release-cache.sh), filled from RS_DRIVER_MIRROR or by one download each.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
odbc="$repo/third_party/redshift_odbc"
rule="$odbc/tools/lib/env_names.py"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp:?}"' EXIT
# shellcheck source=release-cache.sh
source "$here/release-cache.sh"
require_tools python3

n=0; failed=0
pass() { n=$((n+1)); echo "ok $n - $1"; }
fail() { n=$((n+1)); echo "not ok $n - $1"; failed=$((failed+1)); }
# check <name> <policy-dir> <driver> <expected-exit> <substring> [<platform>]: the platform is
# $dp, the platform of the driver the cases below change, unless given. Sets out.
check() {
  local status
  out="$(python3 -I "$rule" "$2" "$3" "${6:-$dp}" 2>&1)" && status=0 || status=$?
  if [[ "$status" == "$4" && "$out" == *"$5"* ]]; then pass "$1"; else fail "$1 (exit $status, wanted $4; output: $out)"; fi
}

base_lock="${RS_DRIVER_LOCK:-$odbc/driver.lock}"
[[ "$base_lock" == /* ]] || base_lock="$PWD/$base_lock"
locked_drivers "$base_lock" "$tmp" > /dev/null # $tmp/<platform>.so per Linux row (release-cache.sh)
real="$odbc/policy"
for p in linux_amd64 linux_arm64; do
  if [[ -f "$tmp/$p.so" ]]; then
    check "the checked-in policy classifies every name in the unmodified $p driver" "$real" "$tmp/$p.so" 0 "scanned names are all classified" "$p"
  else echo "ok - $p # SKIP the lock under test has no $p row"; fi
done
drv="$tmp/linux_amd64.so"
[[ -f "$drv" ]] || drv="$tmp/linux_arm64.so"
# The cases below run on that driver as its own platform, so an arm64-only lock is checked as
# linux_arm64; other is the platform it is not.
dp=linux_amd64 other=linux_arm64
[[ "$drv" == *linux_amd64.so ]] || { dp=linux_arm64 other=linux_amd64; }

# with <name> <string>...: a copy of the driver that also holds each string, NUL-terminated.
with() { local out="$tmp/$1.so"; shift; cp "$drv" "$out"; printf '\0%s\0' "$@" >> "$out"; echo "$out"; }
check "a driver with one more PG name fails, naming it" "$real" "$(with pg PGNEWSETTING)" 1 \
  "environment: the driver names PGNEWSETTING, which no policy list classifies"
check "a driver with one more SSL name fails, naming it" "$real" "$(with ssl SSL_NEW_SETTING)" 1 "the driver names SSL_NEW_SETTING,"
check "a lower-case name under a lower-case prefix fails" "$real" "$(with lower http_proxy_new)" 1 "the driver names http_proxy_new,"
check "a PGRES_ status name is not scanned" "$real" "$(with pgres PGRES_NEW_STATUS)" 0 "all classified"
check "a mixed-case name under a prefix, a function name, is not scanned" "$real" "$(with mixed SSL_new_function)" 0 "all classified"
check "a name at the tail of a longer string is scanned too" "$real" "$(with inner XPGTAILNAME)" 1 "the driver names PGTAILNAME,"
check "every tail is scanned, even inside a longer match" "$real" "$(with nested SSL_PGTAILNAME)" 1 "the driver names PGTAILNAME,"

# policy <name>: a copy of the checked-in policy. Prints its directory.
policy() { local d="$tmp/policy-$1"; mkdir -p "$d"; cp "$real"/*.txt "$d/"; echo "$d"; }
p="$(policy no-known)"; sed -i.orig '/^PGHOST /d' "$p/known-environment.txt"
check "removing a scanned name from the known list fails" "$p" "$drv" 1 "the driver names PGHOST, which no policy list classifies"
p="$(policy no-refused)"; sed -i.orig '/^PGSSLKEY /d' "$p/refused-environment.txt"
check "removing a scanned name from the refused list fails" "$p" "$drv" 1 "the driver names PGSSLKEY, which no policy list classifies"
p="$(policy no-ignored)"; sed -i.orig '/^SSL_ERROR_SYSCALL /d' "$p/ignored-strings.txt"
check "removing an ignored string fails" "$p" "$drv" 1 "the driver names SSL_ERROR_SYSCALL,"
p="$(policy stale-ignored)"; echo "AWS_ERROR_NOT_IN_THE_DRIVER  AWS SDK error name" >> "$p/ignored-strings.txt"
check "an ignored string the driver no longer holds fails" "$p" "$drv" 1 "environment: AWS_ERROR_NOT_IN_THE_DRIVER (ignored-strings.txt:"
p="$(policy stale-known)"; echo "PGNOTINTHEDRIVER  Host" >> "$p/known-environment.txt"
check "a known name the driver no longer holds fails" "$p" "$drv" 1 "environment: PGNOTINTHEDRIVER (known-environment.txt:"
p="$(policy outside)"; echo "OPENSSL_CONF  load" >> "$p/refused-environment.txt"; echo "LD_SOMETHING  -  a reason" >> "$p/known-environment.txt"
check "listed names outside the scanned prefixes do not fail the scan" "$p" "$drv" 0 "all classified"
p="$(policy absent-refused)"; echo "PGNOTINTHEDRIVER  load" >> "$p/refused-environment.txt"
check "a refused name the driver lacks fails" "$p" "$drv" 1 "environment: PGNOTINTHEDRIVER (refused-environment.txt:"
# The "libpq variable, refused even where absent" exemption holds only for the twelve libpq
# variables the refused list always holds, so a typo or a stale row cannot claim it; PGGSSLIB, which
# 2.3.0.0 lacks, is one of them and is exempt in the checked-in policy, which the first cases above
# already pass.
p="$(policy required)"; echo "PGNOTINTHEDRIVER  load  # libpq variable, refused even where absent" >> "$p/refused-environment.txt"
check "another name claiming the libpq exemption fails" "$p" "$drv" 1 \
  "environment: PGNOTINTHEDRIVER (refused-environment.txt:"
# A real refused name outside those twelve cannot claim it either: PGTZ, which this copy of the
# driver no longer holds, must fail even marked as required.
python3 -I -c 'import sys
data = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(data.replace(b"PGTZ", b"pgtz"))' "$drv" "$tmp/no-pgtz.so"
p="$(policy required-pgtz)"; sed -i.orig 's/^PGTZ .*/PGTZ  load  # libpq variable, refused even where absent/' "$p/refused-environment.txt"
check "a refused name outside the twelve cannot claim the exemption" "$p" "$tmp/no-pgtz.so" 1 \
  "environment: PGTZ (refused-environment.txt:"
# A copy of the driver that no longer holds PGHOSTADDR, one of the twelve: with the exemption its
# row may stay, without it the row fails, so the exemption itself is what the case shows.
python3 -I -c 'import sys
data = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(data.replace(b"PGHOSTADDR", b"pghostaddr"))' "$drv" "$tmp/no-hostaddr.so"
p="$(policy required-named)"; sed -i.orig 's/^PGHOSTADDR .*/PGHOSTADDR  load  # libpq variable, refused even where absent/' "$p/refused-environment.txt"
check "one of the twelve libpq variables may be absent, marked so" "$p" "$tmp/no-hostaddr.so" 0 "all classified"
check "the same absent name without the exemption fails" "$real" "$tmp/no-hostaddr.so" 1 \
  "environment: PGHOSTADDR (refused-environment.txt:"
# Every one of the twelve may be absent when marked so: one copy of the driver with all twelve
# lower-cased, and a policy that marks each row.
python3 -I -c 'import sys
data = open(sys.argv[1], "rb").read()
for name in (b"PGGSSLIB", b"PGHOSTADDR", b"PGKRBSRVNAME", b"PGOPTIONS", b"PGREQUIRESSL", b"PGSERVICE",
             b"PGSERVICEFILE", b"PGSSLCERT", b"PGSSLCRL", b"PGSSLKEY", b"PGSSLROOTCERT",
             b"PGSYSCONFDIR"):
    data = data.replace(name, name.lower())
open(sys.argv[2], "wb").write(data)' "$drv" "$tmp/no-twelve.so"
p="$(policy required-twelve)"
for name in PGGSSLIB PGHOSTADDR PGKRBSRVNAME PGOPTIONS PGREQUIRESSL PGSERVICE PGSERVICEFILE \
  PGSSLCERT PGSSLCRL PGSSLKEY PGSSLROOTCERT PGSYSCONFDIR; do
  sed -i.orig "s/^$name .*/$name  load  # libpq variable, refused even where absent/" "$p/refused-environment.txt"
done
# Lower-casing those names also takes away the SSL tails of four of them, which the ignored list
# holds, so this policy copy drops those four rows too.
for row in SSLCERT SSLCRL SSLKEY SSLROOTCERT; do sed -i.orig "/^$row /d" "$p/ignored-strings.txt"; done
check "all twelve libpq variables may be absent, marked so" "$p" "$tmp/no-twelve.so" 0 "all classified"
p="$(policy other-comment)"; echo "PGNOTINTHEDRIVER  load  # some other reason" >> "$p/refused-environment.txt"
check "another comment does not exempt a refused name" "$p" "$drv" 1 "environment: PGNOTINTHEDRIVER (refused-environment.txt:"
# Each exemption is the whole comment, not a phrase inside one.
p="$(policy required-prefixed)"; sed -i.orig 's/^PGHOSTADDR .*/PGHOSTADDR  load  # see: libpq variable, refused even where absent/' "$p/refused-environment.txt"
check "the exemption must be the whole comment" "$p" "$tmp/no-hostaddr.so" 1 "environment: PGHOSTADDR (refused-environment.txt:"
p="$(policy built)"; echo "AWS_ENDPOINT_URL_NEW_SERVICE  load  # built at run time from AWS_ENDPOINT_URL_" >> "$p/refused-environment.txt"
check "a refused name built at run time from a string the driver holds may be absent" "$p" "$drv" 0 "all classified"
p="$(policy built-gone)"; echo "AWS_NOTHING_HERE_SERVICE  load  # built at run time from AWS_NOTHING_HERE_" >> "$p/refused-environment.txt"
check "a refused name built from a string the driver lacks fails" "$p" "$drv" 1 "is built from AWS_NOTHING_HERE_, which is no longer in the driver"
# The base counts when the linker stored it only as the tail of a longer string, as 2.3.0.0 stores
# https_proxy inside idp_use_https_proxy: the driver below holds XPGBASE, so PGBASE is a tail.
p="$(policy built-tail)"; echo "PGBASE  -  a reason" >> "$p/known-environment.txt"
echo "PGBASETAIL  load  # built at run time from PGBASE" >> "$p/refused-environment.txt"
check "a refused name built from a base the driver holds only as a tail may be absent" "$p" "$(with tailbase XPGBASE)" 0 "all classified"
p="$(policy built-trailing)"; echo "AWS_ENDPOINT_URL_NEW  load  # built at run time from AWS_ENDPOINT_URL_ and more" >> "$p/refused-environment.txt"
check "a built-at-run-time comment with words after the string does not exempt" "$p" "$drv" 1 \
  "environment: AWS_ENDPOINT_URL_NEW (refused-environment.txt:"
p="$(policy built-other-name)"; echo "PGNOTINTHEDRIVER  load  # built at run time from AWS_ENDPOINT_URL_" >> "$p/refused-environment.txt"
check "a name that does not start with the string it is built from is not exempt" "$p" "$drv" 1 \
  "environment: PGNOTINTHEDRIVER (refused-environment.txt:"
p="$(policy only-misspelt)"; echo "PGNOTINTHEDRIVER  load  # only in linux_amd46" >> "$p/refused-environment.txt"
check "an \"only in\" row naming an unknown platform fails" "$p" "$drv" 1 \
  "\"only in\" names platforms from linux_amd64, linux_arm64, osx_amd64, osx_arm64"
p="$(policy only-empty)"; echo "PGNOTINTHEDRIVER  load  # only in linux_amd64,,linux_arm64" >> "$p/refused-environment.txt"
check "an \"only in\" row with an empty platform fails" "$p" "$drv" 1 "\"only in\" names platforms from"
p="$(policy only-other)"; echo "PGNOTINTHEDRIVER  load  # only in $other" >> "$p/refused-environment.txt"
check "a refused name only in another platform's driver may be absent" "$p" "$drv" 0 "all classified"
p="$(policy only-this)"; echo "PGNOTINTHEDRIVER  load  # only in $dp" >> "$p/refused-environment.txt"
check "a refused name only in this platform's driver must be found" "$p" "$drv" 1 "environment: PGNOTINTHEDRIVER"
for p in linux_amd64 linux_arm64; do
  [[ -f "$tmp/$p.so" ]] || continue
  check "the $p driver's AWS_COMMON_AVX2, refused only in linux_amd64, matches its platform" "$real" "$tmp/$p.so" 0 "all classified" "$p"
done
# "# only in" on known and ignored rows (macOS): absent elsewhere, and only there.
p="$(policy known-only)"; echo "PGNOTINTHEDRIVER  Host  # only in $other" >> "$p/known-environment.txt"
check "a known name only in another platform's driver may be absent" "$p" "$drv" 0 "all classified"
check "a known name only in this platform's driver must be found" "$p" "$drv" 1 "environment: PGNOTINTHEDRIVER (known-environment.txt:" "$other"
p="$(policy ignored-only)"; echo "AWS_ERROR_NOT_IN_THE_DRIVER  AWS SDK error name  # only in osx_amd64,osx_arm64; a reason" >> "$p/ignored-strings.txt"
check "an ignored string only in other platforms' drivers may be absent, with a reason after the list" "$p" "$drv" 0 "all classified"
p="$(policy known-scope)"; sed -i.orig 's/^PGHOST .*/PGHOST  Host  # only in osx_arm64/' "$p/known-environment.txt"
check "a known name found on a platform its row leaves out fails" "$p" "$drv" 1 \
  "environment: the driver names PGHOST, whose row (known-environment.txt:"
p="$(policy ignored-scope)"; sed -i.orig 's/^SSL_ERROR_SYSCALL .*/SSL_ERROR_SYSCALL  OpenSSL error code name  # only in osx_amd64/' "$p/ignored-strings.txt"
check "an ignored string found on a platform its row leaves out fails" "$p" "$drv" 1 \
  "environment: the driver names SSL_ERROR_SYSCALL, whose row (ignored-strings.txt:"
for list in "linux_amd46" "macos" "," "$dp,"; do
  p="$(policy only-typo)"; echo "PGNOTINTHEDRIVER  Host  # only in $list" >> "$p/known-environment.txt"
  check "a known row's '# only in $list' is a policy problem, not an exemption" "$p" "$drv" 1 "\"only in\" names platforms from"
done
p="$(policy ignored-typo)"; echo "AWS_ERROR_NOT_IN_THE_DRIVER  AWS SDK error name  # only in osx_universal" >> "$p/ignored-strings.txt"
check "an ignored row's misspelt '# only in' is a policy problem" "$p" "$drv" 1 "\"only in\" names platforms from"
check "an unknown platform fails" "$real" "$drv" 1 "environment: osx_universal is not a platform" osx_universal
p="$(policy no-keyword)"; sed -i.orig 's/^PGHOST .*/PGHOST/' "$p/known-environment.txt"
check "a known row without a keyword or a reason fails" "$p" "$drv" 1 "known-environment.txt:"
p="$(policy no-reason)"; sed -i.orig 's/^PGPASSFILE .*/PGPASSFILE  -/' "$p/known-environment.txt"
check "a known - row without a reason fails" "$p" "$drv" 1 "PGPASSFILE has neither a keyword nor a reason"
p="$(policy ignored-no-reason)"; sed -i.orig 's/^SSL_ERROR_SYSCALL .*/SSL_ERROR_SYSCALL/' "$p/ignored-strings.txt"
check "an ignored row without a reason fails" "$p" "$drv" 1 "an ignored row is <string> <reason>; SSL_ERROR_SYSCALL has no reason"
p="$(policy ignored-unscanned)"; echo "OPENSSL_CONF  not a variable" >> "$p/ignored-strings.txt"
check "an ignored row the scan would never report fails" "$p" "$drv" 1 "OPENSSL_CONF is not a string the scan reports"
p="$(policy twice)"; echo "PGHOST  load" >> "$p/refused-environment.txt"
check "a name in two lists fails" "$p" "$drv" 1 "PGHOST is already classified at refused-environment.txt:"
p="$(policy phase)"; sed -i.orig 's/^PGTZ .*/PGTZ  later/' "$p/refused-environment.txt"
check "a refused row with an unknown phase fails" "$p" "$drv" 1 "a refused row is <NAME> <load|handle|connect>"
p="$(policy extra-token)"; sed -i.orig 's/^PGTZ .*/PGTZ  load  extra/' "$p/refused-environment.txt"
check "a refused row with a third field fails" "$p" "$drv" 1 "a refused row is <NAME> <load|handle|connect>"
p="$(policy bad-name)"; sed -i.orig 's/^PGTZ .*/PG-TZ  load/' "$p/refused-environment.txt"
check "a refused row whose name is not a variable name fails" "$p" "$drv" 1 "a refused row is <NAME> <load|handle|connect>"
p="$(policy known-bad-name)"; echo "PG-X  Host" >> "$p/known-environment.txt"
check "a known row whose name is not a variable name fails" "$p" "$drv" 1 "PG-X is not a variable name"
p="$(policy known-keyword)"; sed -i.orig 's/^PGHOST .*/PGHOST  Ho-st/' "$p/known-environment.txt"
check "a known row with a keyword that is not one word fails" "$p" "$drv" 1 "PGHOST has neither a keyword nor a reason"
p="$(policy mixed-prefix)"; echo "Amazon_" >> "$p/environment-prefixes.txt"
check "a mixed-case prefix fails rather than scanning nothing" "$p" "$drv" 1 "a row is one prefix, all upper or all lower case"
p="$(policy two-prefixes)"; echo "XYZ_ ABC_" >> "$p/environment-prefixes.txt"
check "a prefix row with two prefixes fails" "$p" "$drv" 1 "a row is one prefix, all upper or all lower case"
# A policy file or a driver that cannot be read stops the rule with that one problem, rather than
# listing every name the missing input would have classified or held.
# A policy file is read as ASCII: a byte that is not fails the rule rather than being skipped.
p="$(policy non-ascii)"; printf 'SSL_CAF\xc3\xa9  not a variable\n' >> "$p/ignored-strings.txt"
check "a policy file with a byte that is not ASCII fails" "$p" "$drv" 1 "policy: cannot read"
p="$(policy unreadable)"; rm "$p/ignored-strings.txt"
check "a missing policy file fails" "$p" "$drv" 1 "policy: cannot read"
if (( $(wc -l <<<"$out") == 1 )); then pass "a missing policy file is the only problem reported"; else
  fail "a missing policy file reports more: $out"; fi
check "an unreadable driver fails" "$real" "$tmp/no-such.so" 1 "environment: cannot read $tmp/no-such.so"
if (( $(wc -l <<<"$out") == 1 )); then pass "an unreadable driver is the only problem reported"; else
  fail "an unreadable driver reports more: $out"; fi
p="$(policy prefix)"; echo "XYZ_" >> "$p/environment-prefixes.txt"
check "a new prefix scans its names" "$p" "$(with prefix XYZ_SETTING)" 1 "the driver names XYZ_SETTING,"

# same_set <label> <have> <want...>: passes when the lines of <have> are exactly the <want> words.
same_set() {
  local label="$1" have="$2" want missing extra; shift 2
  want="$(printf '%s\n' "$@" | LC_ALL=C sort)"
  have="$(LC_ALL=C sort <<<"$have")"
  missing="$(LC_ALL=C comm -23 <(echo "$want") <(echo "$have") | tr '\n' ' ')"
  extra="$(LC_ALL=C comm -13 <(echo "$want") <(echo "$have") | tr '\n' ' ')"
  if [[ -z "$missing$extra" ]]; then pass "$label"
  else fail "$label (lacks: ${missing% }; also has: ${extra% })"; fi
}

# The checked-in refused list is exactly these rows, at the phases the policy records: the
# Kerberos names and GSS_MECH_CONFIG at load, ODBCINI at handle, every other name at connect. A
# row added, removed or moved to another phase fails here, including a removed per-service
# endpoint row, which the scan cannot notice because the driver builds those names at run time. A
# change to the refused list changes this list too.
reviewed=()
for name in PGGSSLIB PGHOSTADDR PGKRBSRVNAME PGOPTIONS PGREQUIRESSL PGSERVICE PGSERVICEFILE PGSSLCERT \
  PGSSLCRL PGSSLKEY PGSSLROOTCERT PGSYSCONFDIR PGCLIENTENCODING PGDATESTYLE PGTZ PGGEQO PGTTY PGAUTHTYPE \
  PGSSLSNI SSLKEYLOGFILE SSL_CERT_DIR CURL_SSL_BACKEND AWS_PROXY_NO_VERIFY_PEER AWS_CSM_ENABLED AWS_CSM_HOST \
  AWS_CSM_PORT AWS_COMMON_AVX2 AWS_ENDPOINT_URL AWS_ENDPOINT_URL_REDSHIFT AWS_ENDPOINT_URL_REDSHIFT_SERVERLESS \
  AWS_ENDPOINT_URL_SSO AWS_ENDPOINT_URL_SSO_OAUTH AWS_ENDPOINT_URL_SIGNIN AWS_ENDPOINT_URL_STS S2N_INTEG_TEST \
  S2N_DONT_MLOCK; do reviewed+=("$name:connect"); done
for name in KRB5_CONFIG KRB5_TRACE KRB5CCNAME KRB5_KTNAME KRB5_CLIENT_KTNAME KRB5_KDC_PROFILE KRB5RCACHEDIR \
  KRB5RCACHENAME KRB5RCACHETYPE GSS_MECH_CONFIG; do reviewed+=("$name:load"); done
reviewed+=(ODBCINI:handle)
same_set "the refused list is exactly the ${#reviewed[@]} reviewed rows, at their phases" \
  "$(awk '{ sub(/#.*/, "") } NF { print $1 ":" $2 }' "$real/refused-environment.txt")" "${reviewed[@]}"

# The known list's keyword rows are exactly these NAME:Keyword pairs: the client refuses to connect
# when its connection string lacks one, so a pair dropped or changed would leave its variable live
# or check another keyword.
keyword_rows=(SSL_CERT_FILE:CaFile PGDATABASE:Database PGSSLMODE:SSLMode PGAPPNAME:ApplicationName
  PGCONNECT_TIMEOUT:LoginTimeout PGCOMPRESSION:Compression PGUSER:IAM PGPASSWORD:IAM)
same_set "the known list's keyword rows are exactly the ${#keyword_rows[@]} reviewed NAME:Keyword pairs" \
  "$(awk '{ sub(/#.*/, "") } NF && $2 != "-" { print $1 ":" $2 }' "$real/known-environment.txt")" "${keyword_rows[@]}"

# The known rows outside the scanned prefixes, found by reading the driver's source: the scan never
# reports these names, so it cannot notice a removed row, and this pin does. The rows are chosen by the
# scan's own rule, env_names.scanned, so a row the scan does not report cannot fall out of the pin.
# The last five are Windows' counterparts of HOME and TMPDIR.
outside_rows=(HOME CARES_HOSTS HOSTALIASES LOCALDOMAIN RES_OPTIONS OPENSSL_ia32cap OPENSSL_armcap
  AWSLC_CONSOLE_NO_TTY_DETECT S2N_PRINT_STACKTRACE NEGOEX_NO_OPTIMISTIC_TOKEN DISABLE_REQUEST_COMPRESSION
  REQUEST_MIN_COMPRESSION_SIZE_BYTES ec2_metadata_v1_disabled _X_AMZN_TRACE_ID NETRC TMPDIR DISPLAY
  USERPROFILE HOMEDRIVE HOMEPATH TEMP TMP)
# not_scanned <environment-prefixes.txt> <known-environment.txt>: the known names the scan does not report.
not_scanned() {
  python3 -I - "$odbc/tools/lib" "$1" "$2" <<'PY'
import sys
sys.dont_write_bytecode = True  # no __pycache__ in the tree
sys.path.insert(0, sys.argv[1])
import env_names
problems = []
prefixes = [tokens[0] for tokens, _, _ in env_names.rows(sys.argv[2], problems)]
for tokens, _, _ in env_names.rows(sys.argv[3], problems):
    if not env_names.scanned(tokens[0], prefixes):
        print(tokens[0])
sys.exit(1 if problems or not prefixes else 0)
PY
}
if outside="$(not_scanned "$real/environment-prefixes.txt" "$real/known-environment.txt")"; then
  same_set "the known rows outside the scanned prefixes are exactly the ${#outside_rows[@]} the source review found" \
    "$outside" "${outside_rows[@]}"
else
  fail "the known rows outside the scanned prefixes: env_names could not read the policy"
fi

echo "# $n cases, $((n - failed)) passed"
exit $(( failed > 0 ))
