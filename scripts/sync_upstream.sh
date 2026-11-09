#!/bin/bash
# (fork) Brings the official project (NotRexed/Hookshot, main) into this fork's linux-port branch, keeping the fork's own
# additions (docs/fork.md lists them). The fork only adds files, so a sync is normally a merge with no conflicts.
#
#   scripts/sync_upstream.sh            merge, test, and push linux-port
#   scripts/sync_upstream.sh --no-push  merge and test only
#
# On a conflict it stops with the merge left open: resolve, `git commit`, then run it again.
set -euo pipefail
cd "$(dirname "$0")/.."

UPSTREAM_URL=https://github.com/NotRexed/Hookshot.git
BRANCH=linux-port
push=1
[ "${1:-}" = "--no-push" ] && push=0

if ! git remote get-url upstream >/dev/null 2>&1; then
    git remote add upstream "$UPSTREAM_URL"
fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "There are uncommitted changes: commit or stash them first." >&2
    exit 1
fi
git checkout -q "$BRANCH"
git fetch -q upstream main
git fetch -q origin "$BRANCH" 2>/dev/null && git merge -q --ff-only "origin/$BRANCH" 2>/dev/null || true

behind=$(git rev-list --count "HEAD..upstream/main")
if [ "$behind" = 0 ]; then
    echo "Already up to date with the official project."
else
    echo "Merging $behind commit(s) from the official project..."
    if ! git merge --no-edit -m "Merge the official project (NotRexed/Hookshot main) into $BRANCH" upstream/main; then
        echo >&2
        echo "Conflicts in:" >&2
        git diff --name-only --diff-filter=U >&2
        echo "Resolve them, run 'git commit', then run this script again." >&2
        exit 1
    fi
fi

# the fork's test: the hand-written setjmp/longjmp on Linux (tests/setjmp_test.c), against the generated headers
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 -I - "$tmp" <<'EOF'
import os, sys
sys.path.insert(0, "nx2"); sys.path.insert(0, "third_party/a64-kit/lift")
import rtgen, a64_rtgen, a64_lift
out = sys.argv[1]
rtgen.write(os.path.join(out, "rt64"))
os.makedirs(os.path.join(out, "gen"), exist_ok=True)
a64_rtgen.write(os.path.join(out, "gen"))
open(os.path.join(out, "gen", "a64_fpx.h"), "w").write(a64_lift.fpx_header(set()))
EOF
clang -O2 -D_GNU_SOURCE -march=x86-64-v3 -Ihost -Ihost/posix -I"$tmp/rt64" -I"$tmp/gen" -Wno-everything \
    tests/setjmp_test.c -o "$tmp/sjt"
"$tmp/sjt"

if [ "$push" = 1 ]; then
    git push -q origin "$BRANCH"
    echo "Pushed $BRANCH."
fi
echo "Rebuild the game to use the new code: python3 -I nx2/make_game.py ... (the same options as before)."
