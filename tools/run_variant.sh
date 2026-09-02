#!/bin/sh
# Build a variant from a branch and A/B it against the current tree.
#
#   tools/run_variant.sh <branch> [GREP] [RUNS]
#
# Exists so a "does this change help?" question is one command with a
# statistical verdict, rather than an ad-hoc pile of files in /tmp that is
# thrown away with the answer.
set -e
BR="${1:?usage: run_variant.sh <branch> [grep] [runs]}"
GREP="${2:-full leaf}"
RUNS="${3:-25}"
SOD="${SODIUM:-/opt/homebrew/opt/libsodium}"
B="$(mktemp -d)"

# The harness always comes from the CURRENT tree, only the library sources
# come from the variant: a branch created before the harness existed must
# still be measurable, and both sides must run identical benchmark code or
# the comparison means nothing.
HARNESS="$(pwd)/bench/bench_phases.c"
build() {  # build() <src-root> <out>
  cc -O2 -std=c11 -I"$1/include" -I"$1/src" -I"$(pwd)/tests" -D_POSIX_C_SOURCE=200112L \
     "$HARNESS" "$1"/src/core.c "$1"/src/compress.c \
     "$1"/src/const.c "$1"/src/prefix.c -o "$2"
}

git worktree prune
build . "$B/base"
git worktree add -q --detach "$B/wt" "$BR"
build "$B/wt" "$B/var"
echo "== $BR vs working tree =="
python3 tools/ab_compare.py "$B/base" "$B/var" --runs "$RUNS" --grep "$GREP"
git worktree remove --force "$B/wt" 2>/dev/null || true
git worktree prune
rm -rf "$B"
# Leaves no worktree and no branch behind: the --detach above means the
# comparison never claims a branch, so nothing blocks deleting one later.
