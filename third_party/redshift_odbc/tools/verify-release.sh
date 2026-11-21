#!/usr/bin/env bash
# Proves that a driver release is AWS's and fits the reviewed loading policy before a lock names it.
# On Linux (lib/verify_linux.sh): the pinned intermediate, the chain, the Code Signing usage, the
# subject and the detached signature; that the signed package is the version and architecture asked
# for; the driver taken out with the member checks; the notices from the release tag against the
# checked-in copies, a package that holds one failing the run; the Linux loading policy and the
# environment-name rule. On macOS (lib/verify_macos.sh), from Linux too: the universal package's
# signature through the pinned Apple root and Developer ID intermediate, and that the signed package
# is the driver at the version asked for; the library taken out of its payload with the same member
# checks; the notices as on Linux; the macOS loading policy and the environment-name rule on each
# slice. On Windows (lib/verify_windows.sh), from Linux too: the MSI's Authenticode signature
# through the intermediate Linux pins, and its tables and cabinet, from which the set's files are
# taken; the notices as on Linux; the Windows loading policy and the environment-name rule. A
# failure names its rule. Only then does it print the lock rows, the only output on stdout: the
# platform's on Linux, on macOS both osx_amd64's and osx_arm64's, which name the one universal
# library and package and so come from one run, and on Windows a row per file of the set.
#
#   verify-release.sh <version> <platform>
#
# The certificates it trusts are the ones under signing/ and, on Linux, the system trust store (a
# macOS chain is anchored at the pinned Apple root alone), and the policy is the one under policy/:
# no option changes either, and the variables that could are unset before openssl or a Python helper
# runs (tools/lib/common.sh, no_trust_variables). The chain is checked at the current time, so a
# release whose signing certificate has expired is not verified again; it is instead reused from the
# recorded lock and the retained package (a driver rollback).
# Two variables say where the bytes come from, never what is trusted:
#   RS_DRIVER_MIRROR  a directory of retained release files (the driver mirror),
#                     read instead of downloading when RS_DRIVER_LOCK names this version and
#                     platform (the README's "What verification proves" gives the rule).
#   RS_DRIVER_LOCK    the recorded lock, read through check-lock.sh.
# Everything else a host can set (PATH, LD_PRELOAD, BASH_ENV, TMPDIR) reaches any command, here as
# anywhere, so run it with a clean environment and a private temporary directory. The rules are in
# tools/lib/, which take their trust anchors as arguments so the tests can pass a test PKI; this
# command always passes the repository's. Progress and failures go to stderr; a failure names the
# rule.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
usage() { echo "usage: verify-release.sh <version> <platform>" >&2; exit 2; }
(($# == 2)) || usage
version="$1" platform="$2"
case "$platform" in
linux_amd64 | linux_arm64 | windows_amd64) ;;
osx_amd64 | osx_arm64)
  # Either one verifies the universal package both macOS platforms embed, and prints both rows. Its
  # chain is anchored at the pinned Apple root, not at the system trust store.
  exec bash "$here/lib/verify_macos.sh" "$platform" "$version" "$here/.." "$here/../../../src/transport/odbc_driver.hpp" ;;
*) echo "verify-release: unknown platform $platform" >&2; exit 1 ;;
esac

# The system trust store, which must hold DigiCert Trusted Root G4: the distribution's CA bundle,
# from a fixed list of the usual locations, never from the environment. With none, verify_linux.sh
# and verify_windows.sh fail before any download (their step 1).
bundle=""
for b in /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem; do
  if [[ -f "$b" ]]; then bundle="$b"; break; fi
done
verifier=verify_linux.sh
if [[ "$platform" == windows_amd64 ]]; then verifier=verify_windows.sh; fi
exec bash "$here/lib/$verifier" "$platform" "$version" "$here/.." "$here/../../../src/transport/odbc_driver.hpp" "$bundle"
