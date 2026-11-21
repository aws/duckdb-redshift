#!/usr/bin/env bash
# Fixture tests for scripts/clone-duckdb.sh (make duckdb), against a planted DuckDB remote, and checks
# of the files around it: the pin, the .gitignore entries, the absence of submodules, the vendored
# extension-ci-tools Makefile and the root Makefile's duckdb target, its version stamp and its warning
# for a duckdb/ off the pin. Run with `make test_clone_duckdb`. Needs git and no network.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
package="$(cd "$here/.." && pwd)"
script="$package/scripts/clone-duckdb.sh"
# Resolved, as make's own directory is (macOS's TMPDIR is under the link /var and ends in a slash),
# so that the paths the script and make print are the ones the cases expect.
tmp="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf -- "${tmp:?}"' EXIT

# git without the caller's configuration, which could sign commits, rewrite URLs or run hooks.
mkdir "$tmp/home"
export HOME="$tmp/home" GIT_CONFIG_NOSYSTEM=1
unset XDG_CONFIG_HOME GIT_DIR GIT_WORK_TREE
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com

n=0; failed=0
ok() { n=$((n+1)); echo "ok $n - $1"; }
not_ok() { n=$((n+1)); failed=$((failed+1)); echo "not ok $n - $1"; }
check() { local name="$1"; shift; if "$@"; then ok "$name"; else not_ok "$name"; fi; }
head_of() { git -C "$1" rev-parse HEAD 2>/dev/null; }

# The remote: the pinned commit with a lightweight tag, as DuckDB tags its releases, one with an
# annotated tag, and a later commit, the branch head, with a tag of its own.
up="$tmp/upstream"
git init -q "$up"
echo 'project(duckdb)' > "$up/CMakeLists.txt"
git -C "$up" add CMakeLists.txt
git -C "$up" commit -q -m pinned
git -C "$up" tag v9.9.9
pin="$(head_of "$up")"
echo 'annotated' > "$up/a"
git -C "$up" add a
git -C "$up" commit -q -m annotated
git -C "$up" tag -a v8.8.8 -m v8.8.8
annotated="$(head_of "$up")"
echo 'later' > "$up/b"
git -C "$up" add b
git -C "$up" commit -q -m later
git -C "$up" tag later
later="$(head_of "$up")"

# A package root holding duckdb.commit; a git work tree of its own, as a checkout of this repository is.
new_package() {
	mkdir "$tmp/$1"
	git init -q "$tmp/$1"
	echo "$2" > "$tmp/$1/duckdb.commit"
	echo "$tmp/$1"
}
# Runs the script on package root $1, cloning from $remote (default the remote above); sets status
# and out.
run() {
	out="$(DUCKDB_GIT_REPOSITORY="${remote:-file://$up}" bash "$script" "$@" 2>&1)" && status=0 || status=$?
}
# A full clone of the remote at commit $2, in $tmp/$1, for a link to point at.
tree_at() {
	git clone -q "file://$up" "$tmp/$1"
	git -C "$tmp/$1" checkout -q --detach "$2"
}

pkg="$(new_package pkg "$pin")"
run "$pkg"
check "clones the pinned commit, not the branch head" \
  test "$status" -eq 0 -a "$(head_of "$pkg/duckdb")" = "$pin" -a "$later" != "$pin"
check "fetches the tag at the pin, which the Makefile's OVERRIDE_GIT_DESCRIBE reads" \
  test "$(git -C "$pkg/duckdb" describe --tags --exact-match 2>&1)" = v9.9.9
check "fetches no tag of another commit" test -z "$(git -C "$pkg/duckdb" tag -l later)"
check "says what it cloned" test "$out" = "clone-duckdb: cloned $pin (v9.9.9) from file://$up into $pkg/duckdb"

echo 'mine' > "$pkg/duckdb/marker"
run "$pkg"
check "accepts duckdb/ at the pin, and leaves it as it is" \
  test "$status" -eq 0 -a "$out" = "clone-duckdb: $pkg/duckdb is already at $pin" -a -f "$pkg/duckdb/marker"

git -C "$pkg/duckdb" fetch -q --depth 1 origin "$later"
git -C "$pkg/duckdb" checkout -q --detach "$later"
run "$pkg"
check "refuses duckdb/ at another commit, and leaves it there" \
  test "$status" -eq 1 -a "$(head_of "$pkg/duckdb")" = "$later" -a -f "$pkg/duckdb/marker"
check "names both commits when it refuses" bash -c '[[ $1 == *"at $2, but duckdb.commit pins $3"* ]]' _ "$out" "$later" "$pin"

pkg="$(new_package annotated "$annotated")"
run "$pkg"
check "fetches an annotated tag at the pin" \
  test "$status" -eq 0 -a "$(git -C "$pkg/duckdb" describe --tags --exact-match 2>&1)" = v8.8.8
check "fetches the pin alone, not its history" bash -c '! git -C "$1" cat-file -e "$2^{commit}" 2>/dev/null' _ "$pkg/duckdb" "$pin"

tree_at tree-at-pin "$pin"
pkg="$(new_package linked "$pin")"
ln -s "$tmp/tree-at-pin" "$pkg/duckdb"
run "$pkg"
check "accepts a link to a DuckDB tree at the pin, and keeps the link" test "$status" -eq 0 -a -L "$pkg/duckdb"
tree_at tree-later "$later"
pkg="$(new_package linked-later "$pin")"
ln -s "$tmp/tree-later" "$pkg/duckdb"
run "$pkg"
check "refuses a link to a DuckDB tree at another commit" test "$status" -eq 1 -a "$(head_of "$tmp/tree-later")" = "$later"
pkg="$(new_package dangling "$pin")"
ln -s "$tmp/nowhere" "$pkg/duckdb"
run "$pkg"
check "refuses a link that reaches nothing, and keeps it" test "$status" -eq 1 -a -L "$pkg/duckdb"
pkg="$(new_package no-commit "$pin")"
git init -q "$pkg/duckdb"
run "$pkg"
check "refuses a clone of its own with no commit, says so, and keeps it" \
  bash -c '[[ $1 == 1 && $2 == *"$3/duckdb is at no commit, but duckdb.commit pins $4"* && -d $3/duckdb/.git ]]' _ "$status" "$out" "$pkg" "$pin"
tree_at tree-untagged "$pin"
git -C "$tmp/tree-untagged" tag -d v9.9.9 >/dev/null
pkg="$(new_package untagged "$pin")"
ln -s "$tmp/tree-untagged" "$pkg/duckdb"
run "$pkg"
check "accepts a tree at the pin without its tag, and warns that builds will not carry the version" \
  bash -c '[[ $1 == 0 && $2 == *"warning: $3/duckdb has no tag at $4"* ]]' _ "$status" "$out" "$pkg" "$pin"
bare="$tmp/untagged.git"
git clone -q --bare "file://$up" "$bare"
git -C "$bare" tag -d v9.9.9 >/dev/null
pkg="$(new_package untagged-remote "$pin")"
remote="file://$bare" run "$pkg"
check "clones from a remote without the pin's tag, and warns" \
  bash -c '[[ $1 == 0 && $2 == *"warning: $3/duckdb has no tag at $4"* ]]' _ "$status" "$out" "$pkg" "$pin"

# A git hook or `git rebase -x` exports GIT_DIR for the package's own repository.
pkg="$(new_package inherited "$pin")"
git -C "$pkg" add duckdb.commit
git -C "$pkg" commit -q -m package
own="$(head_of "$pkg")"
GIT_DIR="$pkg/.git" run "$pkg"
check "clones into duckdb/ under a GIT_DIR inherited for the package's repository, and leaves that repository alone" \
  test "$status" -eq 0 -a "$(head_of "$pkg/duckdb")" = "$pin" -a "$(head_of "$pkg")" = "$own" -a -z "$(git -C "$pkg" remote)"
pkg="$tmp/linked"
GIT_DIR="$pkg/.git" run "$pkg"
check "accepts a tree at the pin under such a GIT_DIR, rather than reading the package's HEAD" \
  test "$status" -eq 0 -a "$out" = "clone-duckdb: $pkg/duckdb is already at $pin"

# Inside the package's work tree, with a commit, git -C duckdb would read the package's own HEAD.
pkg="$(new_package not-a-clone "$pin")"
git -C "$pkg" add duckdb.commit
git -C "$pkg" commit -q -m package
mkdir "$pkg/duckdb"
echo 'x' > "$pkg/duckdb/f"
run "$pkg"
check "refuses a directory that is not a clone, and says so rather than naming the package's commit" \
  bash -c '[[ $1 == 1 && $2 == *"$3/duckdb is not a DuckDB clone"* && -f $3/duckdb/f && ! -e $3/duckdb/.git ]]' _ "$status" "$out" "$pkg"
pkg="$(new_package empty "$pin")"
mkdir "$pkg/duckdb"
run "$pkg"
check "clones into an empty duckdb/, as a checkout from before the submodules were removed may leave" \
  test "$status" -eq 0 -a "$(head_of "$pkg/duckdb")" = "$pin"

pkg="$(new_package short "${pin:0:12}")"
run "$pkg"
check "refuses an abbreviated pin, and clones nothing" test "$status" -eq 2 -a ! -e "$pkg/duckdb"
pkg="$(new_package two-lines "$pin")"
echo "$later" >> "$pkg/duckdb.commit"
run "$pkg"
check "refuses a pin file of two lines" test "$status" -eq 2 -a ! -e "$pkg/duckdb"
pkg="$(new_package crlf "$pin"$'\r')"
run "$pkg"
check "accepts a pin whose line ends in CRLF, as a checkout under core.autocrlf=true writes it" \
  test "$status" -eq 0 -a "$(head_of "$pkg/duckdb")" = "$pin"
pkg="$(new_package missing "$pin")"
rm "$pkg/duckdb.commit"
run "$pkg"
check "refuses a package root without duckdb.commit" test "$status" -eq 2 -a ! -e "$pkg/duckdb"
# A tag at the pin whose name holds a character outside the safe set ([A-Za-z0-9._-]): the build
# passes the tag to cmake through the shell, so only safe names are fetched. The placeholder is
# harmless on purpose; no test here builds a name the shell would run.
git -C "$up" tag 'v9.9.9+placeholder' "$pin"
pkg="$(new_package unsafe-tag "$pin")"
run "$pkg"
git -C "$up" tag -d 'v9.9.9+placeholder' >/dev/null
check "fetches only the tags whose names hold only safe characters" \
  test "$status" -eq 0 -a "$(git -C "$pkg/duckdb" tag -l)" = v9.9.9
# A remote that serves the clone's fetch and refuses every later request, the tag listing first.
cat > "$tmp/upload-pack-once" <<'SERVE'
#!/usr/bin/env bash
mkdir "$0.served" 2>/dev/null || { touch "$0.refused"; exit 1; }
exec git upload-pack "$@"
SERVE
chmod +x "$tmp/upload-pack-once"
pkg="$(new_package listing-fails "$pin")"
GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=remote.origin.uploadpack GIT_CONFIG_VALUE_0="$tmp/upload-pack-once" run "$pkg"
check "fails when the tag listing fails after the fetch, and leaves no duckdb/" \
  test "$status" -ne 0 -a ! -e "$pkg/duckdb" -a -d "$tmp/upload-pack-once.served" -a -e "$tmp/upload-pack-once.refused"
# An annotated tag's own id, which git ls-remote --tags lists first: the fetch takes the tag and the
# checkout lands on its commit, so the clone is not at the pin.
pkg="$(new_package tag-id "$(git -C "$up" rev-parse v8.8.8)")"
run "$pkg"
check "refuses a pin that names an annotated tag rather than its commit, and leaves no duckdb/" \
  bash -c '[[ $1 == 1 && $2 == *"is not a commit at "*" but a tag of $3;"* && ! -e $4/duckdb ]]' _ "$status" "$out" "$annotated" "$pkg"
pkg="$(new_package unknown 0123456789abcdef0123456789abcdef01234567)"
run "$pkg"
check "fails on a pin the remote does not have, and leaves no duckdb/" test "$status" -ne 0 -a ! -e "$pkg/duckdb"
run
check "a missing argument is a usage error" test "$status" -eq 2

# The files around the script, in this repository.
check "this package's duckdb.commit is one full commit id" \
  bash -c '[[ "$(cat "$1")" =~ ^[0-9a-f]{40}$ ]]' _ "$package/duckdb.commit"
ignored="$tmp/ignored"
git init -q "$ignored"
cp "$package/.gitignore" "$ignored/"
ln -s "$tmp/tree-at-pin" "$ignored/duckdb"
check "the .gitignore ignores duckdb/ as a link" git -C "$ignored" check-ignore -q duckdb
rm "$ignored/duckdb"
mkdir -p "$ignored/duckdb"
echo 'x' > "$ignored/duckdb/f"
check "the .gitignore ignores duckdb/ as a directory" git -C "$ignored" check-ignore -q duckdb/f
check "this package has no .gitmodules" test ! -e "$package/.gitmodules"
if prefix="$(git -C "$package" rev-parse --show-prefix 2>/dev/null)" && [[ -z "$prefix" ]]; then
	gitlinks="$(git -C "$package" ls-files -s | awk '$1 == "160000"')"
	check "this package's index holds no submodule" test -z "$gitlinks"
else
	echo "# this package is not a git work tree of its own: skipping the index check"
fi

vendored="$package/third_party/extension-ci-tools/duckdb_extension.Makefile"
readme="$package/third_party/extension-ci-tools/README.md"
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi | cut -d ' ' -f 1; }
recorded="$(sed -n 's/^\([0-9a-f]\{64\}\)  makefiles\/duckdb_extension.Makefile$/\1/p' "$readme")"
check "the vendored Makefile below its first line is the upstream file the README records" \
  test -n "$recorded" -a "$(tail -n +2 "$vendored" | sha256)" = "$recorded"
commit="$(sed -n 's/^- Commit: `\([0-9a-f]\{40\}\)`.*/\1/p' "$readme")"
check "the vendored Makefile's first line names the commit the README records" \
  bash -c '[[ -n $2 && "$(head -n 1 "$1")" == "# From https://github.com/duckdb/extension-ci-tools/blob/$2/makefiles/duckdb_extension.Makefile, "* ]]' \
  _ "$vendored" "$commit"

# .clang-format and .clang-tidy are DuckDB's at the pin, copied because clang-format and clang-tidy
# read only the directories above each source file, so a pin move that skips the copy leaves them
# stale. Compared with duckdb/'s byte for byte where duckdb/ is a clone of its own at the pin, as
# make duckdb leaves it; anywhere else there is nothing at the pin to compare them with.
if [[ -z "$(git -C "$package/duckdb" rev-parse --show-prefix 2>/dev/null || echo none)" ]] &&
	[[ "$(git -C "$package/duckdb" rev-parse -q --verify HEAD 2>/dev/null)" == "$(cat "$package/duckdb.commit")" ]]; then
	for settings in .clang-format .clang-tidy; do
		check "this package's $settings is duckdb/'s at the pin" cmp -s "$package/$settings" "$package/duckdb/$settings"
	done
else
	echo "# duckdb/ is not a clone at duckdb.commit: skipping the check of .clang-format and .clang-tidy against it"
fi

# The Makefile's duckdb target, in a planted tree with this Makefile, the vendored one and a duckdb/
# directory: make must run the script although duckdb/ exists, and the default goal stays the
# vendored Makefile's release build. The makes from here on run without a caller's
# OVERRIDE_GIT_DESCRIBE, as a CI job may set one, which takes the place of the version the Makefile
# reads from duckdb/ (its ?=); one case below sets its own.
unset OVERRIDE_GIT_DESCRIBE
mk="$tmp/mk"
mkdir -p "$mk/third_party/extension-ci-tools" "$mk/duckdb"
cp "$package/Makefile" "$mk/"
cp "$vendored" "$mk/third_party/extension-ci-tools/"
plan="$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" duckdb 2>&1)"
check "make duckdb runs the script although duckdb/ exists" test "$plan" = "bash $mk/scripts/clone-duckdb.sh $mk/"
plan="$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" 2>&1)"
check "make with no target is still the release build, and does not clone DuckDB" \
  bash -c '[[ $1 == *"-B build/release"* && $1 != *clone-duckdb* ]]' _ "$plan"
# And in this tree, with whatever makefiles make reads here first, as a source tree's own.
plan="$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$package" 2>&1)"
check "in this tree, make with no target is the release build" bash -c '[[ $1 == *"-B build/release"* ]]' _ "$plan"

# Builds compile duckdb/ as it is, and warn when that clone is at another commit than the pin.
echo "$pin" > "$mk/duckdb.commit"
rmdir "$mk/duckdb"
ln -s "$tmp/tree-later" "$mk/duckdb"
plan="$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" 2>&1)"
check "a build warns when duckdb/ is at another commit than duckdb.commit pins" \
  bash -c '[[ $1 == *"duckdb/ is at $2, but duckdb.commit pins $3"* ]]' _ "$plan" "$later" "$pin"
rm "$mk/duckdb"
ln -s "$tmp/tree-at-pin" "$mk/duckdb"
plan="$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" 2>&1)"
check "and does not warn when it is at the pin" bash -c '[[ $1 != *"duckdb.commit pins"* ]]' _ "$plan"
# make's $(shell) drops the CR before a newline, so its reading of the pin needs no change for it.
echo "$pin"$'\r' > "$mk/duckdb.commit"
plan="$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" 2>&1)"
check "nor when duckdb.commit's line ends in CRLF" bash -c '[[ $1 != *"duckdb.commit pins"* ]]' _ "$plan"
echo "$pin" > "$mk/duckdb.commit"
# Under a GIT_DIR inherited for the package's repository, which has a commit and a tag of its own, a
# git run in duckdb/ that kept it would read the package's HEAD.
git init -q "$mk"
git -C "$mk" add Makefile
git -C "$mk" commit -q -m package
git -C "$mk" tag v0.0.0-package
plan="$(GIT_DIR="$mk/.git" env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" 2>&1)"
check "under a GIT_DIR inherited for the package's repository, the build still takes its version from duckdb/'s tag" \
  bash -c '[[ $1 == *"-DOVERRIDE_GIT_DESCRIBE=\"v9.9.9\""* ]]' _ "$plan"
check "and does not take the package's HEAD for duckdb/'s, so does not warn" bash -c '[[ $1 != *"duckdb.commit pins"* ]]' _ "$plan"
# A caller's OVERRIDE_GIT_DESCRIBE, as a CI job may set one, takes the place of duckdb/'s tag.
plan="$(OVERRIDE_GIT_DESCRIBE=v7.7.7 env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -n --no-print-directory -C "$mk" 2>&1)"
check "a caller's OVERRIDE_GIT_DESCRIBE wins over duckdb/'s tag" \
  bash -c '[[ $1 == *"-DOVERRIDE_GIT_DESCRIBE=\"v7.7.7\""* && $1 != *"v9.9.9"* ]]' _ "$plan"
rm "$mk/duckdb"
ln -s "$tmp/tree-later" "$mk/duckdb"
plan="$(env -u MAKEFLAGS -u MFLAGS MAKELEVEL=1 make -s -n --no-print-directory -C "$mk" 2>&1)"
check "a sub-make does not warn again" bash -c '[[ $1 != *"duckdb.commit pins"* ]]' _ "$plan"

echo "# $n cases, $((n - failed)) passed"
[[ $failed -eq 0 ]]
