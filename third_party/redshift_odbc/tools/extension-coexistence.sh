#!/usr/bin/env bash
# extension-coexistence.sh <duckdb CLI> <redshift.duckdb_extension, or "" when the CLI has it built in> <extension>...
# The redshift extension next to other DuckDB extensions: loads each <extension> (a path to a
# .duckdb_extension file, or the name of one built into the CLI: each order runs with an empty HOME,
# so nothing is installed) before the redshift extension and then after it, and connects each
# time, with the statements in connect.sql beside this script, which run-e2e.sh runs too. Typical
# inputs are the postgres, httpfs and aws extensions and the community odbc extension, as local
# files; this script downloads nothing. It must run where nothing can
# leave the host, which it checks as run-e2e.sh does: the connection goes to the endpoint the region
# derives, so a loaded driver fails the connect itself. Each order passes when the connect fails
# that way and the driver set was written to its own REDSHIFT_DRIVER_DIR. With "" for the
# redshift extension (a CLI that links it in) there is one order, `loaded`. The CLI is a
# path, or a name found on PATH; an extension is a path when DuckDB's LOAD takes it as one (it holds
# a '.', '/' or '\'), made absolute, as each order runs in a directory of its own. Prints
# `<order>\t<ok|fail>\t<detail>` per order, the detail on one line and cut to 300 bytes; exits 1 if
# one failed (an extension file that is not there fails its order), and 2, before any order runs, on
# a usage error, a CLI that is not an executable file, a path whose directory cannot be entered, a
# missing or empty connect.sql, or a network.
set -euo pipefail
refuse() { echo "extension-coexistence: $1" >&2; exit 2; }
[[ $# -ge 3 ]] || refuse "usage: extension-coexistence.sh <duckdb CLI> <redshift extension or \"\"> <extension>..."
# absolute <path>: the path with its directory made absolute; exit 2 when the directory cannot be entered.
absolute() {
  local dir
  dir="$(cd "$(dirname "$1")" 2>/dev/null && pwd)" || refuse "cannot enter the directory of $1"
  printf '%s/%s' "$dir" "$(basename "$1")"
}
if [[ "$1" == */* ]]; then
  cli="$(absolute "$1")" || exit 2
else
  cli="$(command -v -- "$1")" && [[ "$cli" == /* ]] || refuse "no program named $1 on PATH"
fi
[[ -f "$cli" && -x "$cli" ]] || refuse "the CLI $cli is not an executable file"
redshift="$2"; shift 2
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
env -u REDSHIFT_ALLOW_NETWORK bash "$here/../../../test/client/odbc/check-no-network.sh" >/dev/null 2>&1 ||
  refuse "this process has a network; run it in a container with --network none"
connect="$(cat "$here/connect.sql" 2>/dev/null)" && [[ -n "$connect" ]] || refuse "no connect.sql, or an empty one, beside this script"
work="$(mktemp -d)"
work="$(cd "$work" && pwd -P)"
trap 'rm -rf -- "${work:?}"' EXIT
chmod 700 "$work"
quote="'" quotes="''"
# A LOAD of the path, made absolute, or of the name, with each quote doubled. DuckDB's LOAD takes an argument
# with a '.', '/' or '\' as a path (ExtensionHelper::IsFullPath).
load() {
  local target="$1"
  if [[ "$target" == *[./\\]* ]]; then target="$(absolute "$target")" || exit 2; fi
  printf "LOAD '%s'; " "${target//$quote/$quotes}"
}
others=""
for extension in "$@"; do others+="$(load "$extension")"; done
own=""
[[ -z "$redshift" ]] || own="$(load "$redshift")"
status=0
orders=(others-first redshift-first)
[[ -n "$redshift" ]] || orders=(loaded)
for order in "${orders[@]}"; do
  if [[ "$order" == redshift-first ]]; then loads="$own$others"; else loads="$others$own"; fi
  mkdir -m 700 "$work/driver-$order" "$work/home-$order"
  out="$(cd "$work" && env -i PATH="$PATH" HOME="$work/home-$order" REDSHIFT_DRIVER_DIR="$work/driver-$order" \
    AWS_ACCESS_KEY_ID=test-not-a-key AWS_SECRET_ACCESS_KEY=test-not-a-secret AWS_REGION=us-east-1 \
    AWS_EC2_METADATA_DISABLED=true "$cli" -unsigned -c "$loads$connect" 2>&1)" && rc=0 || rc=$?
  # One field: tabs and line ends become spaces, so the line keeps its three fields.
  detail="$(tr '\n\t' '  ' <<<"$out" | cut -c1-300)"
  # The CLI exits 1 on an SQL error; anything else (a crash, a signal) fails the order.
  if ((rc == 1)) && [[ "$out" == *"Could not connect to Redshift at "*": the connection attempt failed."* ]] &&
    [[ -d "$work/driver-$order/.redshift-driver" ]]; then
    printf '%s\tok\t%s\n' "$order" "$detail"
  else
    printf '%s\tfail\t%s\n' "$order" "$detail"
    status=1
  fi
done
exit "$status"
