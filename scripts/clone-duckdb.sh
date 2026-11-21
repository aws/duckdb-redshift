#!/usr/bin/env bash
# Clones DuckDB at the commit duckdb.commit pins into duckdb/, the tree the build compiles against.
# The Makefile's duckdb target runs it (make duckdb).
#
#   clone-duckdb.sh <package root>
#
# The extension must be built against exactly that commit, so a duckdb/ that is already there (an
# earlier clone, or a link to a DuckDB tree elsewhere) is accepted when its HEAD is the pinned commit
# and refused otherwise: it is never moved or replaced. An empty directory counts as no tree. The
# clone is shallow, the commit and the tags that point at it: the Makefile's OVERRIDE_GIT_DESCRIBE
# reads the tag, so that the build carries DuckDB's version (v2.0.1) rather than a commit, which a
# stock DuckDB of that version refuses to LOAD; a duckdb/ without such a tag gets a warning.
# `git -C duckdb fetch --unshallow` adds the history.
# DUCKDB_GIT_REPOSITORY names another remote to clone from (default DuckDB's on GitHub).
set -euo pipefail
# A git hook or `git rebase -x` exports GIT_DIR and its kin for this repository, which
# would make git -C duckdb read and write that one. Drop them as git does when it enters a
# submodule, keeping the caller's -c settings (GIT_CONFIG_PARAMETERS, GIT_CONFIG_COUNT). The
# Makefile's DROP_GIT_ENV does the same for its own git runs in duckdb/; keep the two alike.
for var in $(git rev-parse --local-env-vars); do
	[[ $var == GIT_CONFIG_PARAMETERS || $var == GIT_CONFIG_COUNT ]] || unset "$var"
done

[[ $# -eq 1 ]] || { echo "usage: clone-duckdb.sh <package root>" >&2; exit 2; }
root="$(cd "$1" && pwd)"
pin_file="$root/duckdb.commit"
[[ -f "$pin_file" ]] || { echo "clone-duckdb: $pin_file is missing" >&2; exit 2; }
# One full lowercase commit id: a fetch by id needs all of it, and an abbreviation could later
# match another commit too. A checkout under core.autocrlf=true ends the line with CRLF, so the CR
# is dropped first; the Makefile's $(shell) drops it itself.
pin="$(tr -d '\r' < "$pin_file")"
[[ "$pin" =~ ^[0-9a-f]{40}$ ]] || { echo "clone-duckdb: $pin_file must hold one full commit id, not '$pin'" >&2; exit 2; }
repository="${DUCKDB_GIT_REPOSITORY:-https://github.com/duckdb/duckdb.git}"
dest="$root/duckdb"
# Warns when duckdb/ has no tag at the pin: the build then carries a commit rather than DuckDB's
# version, which a stock DuckDB refuses to LOAD. A warning, as builds and in-tree tests still work.
warn_untagged() {
	git -C "$dest" describe --tags --exact-match >/dev/null 2>&1 ||
		echo "clone-duckdb: warning: $dest has no tag at $pin, so builds will not carry DuckDB's version;" \
			"remove it and run make duckdb again, from a remote that has the tag" >&2
}

if [[ -d "$dest" && ! -L "$dest" && -z "$(ls -A "$dest")" ]]; then
	rmdir "$dest"
fi
if [[ -e "$dest" || -L "$dest" ]]; then
	# Its own work tree (an empty prefix), and not a directory inside this repository's, where git
	# would read this repository's HEAD instead. The prefix, unlike a top-level path, has one form
	# under Git for Windows and Git bash alike.
	prefix="$(git -C "$dest" rev-parse --show-prefix 2>/dev/null)" || prefix=none
	if [[ -n "$prefix" ]]; then
		echo "clone-duckdb: $dest is not a DuckDB clone; remove it and run make duckdb again" >&2
		exit 1
	fi
	current="$(git -C "$dest" rev-parse -q --verify HEAD 2>/dev/null)" || current=
	if [[ "$current" != "$pin" ]]; then
		echo "clone-duckdb: $dest is at ${current:-no commit}, but duckdb.commit pins $pin;" \
			"remove it (or point the link at a DuckDB tree at that commit) and run make duckdb again" >&2
		exit 1
	fi
	echo "clone-duckdb: $dest is already at $pin"
	warn_untagged
	exit 0
fi

# Into duckdb/ itself, removed again if any step fails, so a failed clone leaves nothing that a
# later run or the build could take for a tree.
trap 'rm -rf -- "${dest:?}"' EXIT
git init -q "$dest"
git -C "$dest" remote add origin "$repository"
git -C "$dest" fetch -q --depth 1 --no-tags origin "$pin"
git -C "$dest" checkout -q --detach FETCH_HEAD
# A pin that names an annotated tag, whose id `git ls-remote --tags` lists before its commit's,
# fetches the tag and checks out the commit it points at.
head="$(git -C "$dest" rev-parse HEAD)"
[[ "$head" == "$pin" ]] || { echo "clone-duckdb: $pin is not a commit at $repository but a tag of $head; put that commit in duckdb.commit" >&2; exit 1; }
# The tags that point at the pin: a lightweight tag's line names the commit, an annotated tag's
# peeled line (its name ending ^{}) does. Listed first, so a failed listing fails the clone rather
# than leaving a tree without its version. Only names of letters, digits, '.', '_' and '-': the
# build passes the tag to cmake through the shell (OVERRIDE_GIT_DESCRIBE), and a ref name may hold
# $(, ) and backquotes.
refs="$(git -C "$dest" ls-remote --tags origin)"
tags=
for tag in $(awk -v pin="$pin" '$1 == pin { sub("^refs/tags/", "", $2); sub("\\^\\{\\}$", "", $2); if ($2 ~ /^[A-Za-z0-9._-]+$/) print $2 }' <<<"$refs" | sort -u); do
	# No --depth: the commit is already here, and with it GitHub would send the whole tree again.
	git -C "$dest" fetch -q --no-tags origin "refs/tags/$tag:refs/tags/$tag"
	tags="${tags:+$tags, }$tag"
done
trap - EXIT
echo "clone-duckdb: cloned $pin${tags:+ ($tags)} from $repository into $dest"
[[ -n "$tags" ]] || warn_untagged
