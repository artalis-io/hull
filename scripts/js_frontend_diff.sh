#!/bin/sh
# js_frontend_diff.sh - JS-mode frontend output, merge base vs this checkout.
#
#   sh scripts/js_frontend_diff.sh [BASE_REF]        (make js-frontend-diff [BASE=ref])
#
# The TypeScript work (docs/typescript_design.md section 3) adds a TypeScript mode to the
# JS lexer and parser, and JS mode must not change. test_js_golden freezes a fixed input
# set; this script covers the repo JS that ordinary PRs edit: it builds the dump tool
# (tests/hull/frontend/js_frontend_dump.c) against the frontend at BASE_REF (default: the
# merge base with origin/main) and against this checkout, runs both over the SAME inputs
# (this checkout's files), and fails on any byte of difference in the lex / parse /
# resolveScope output. It also prints both timings (a slowdown is for the PR to explain).
#
# Skips (exit 0) when nothing that can change JS-mode output differs from BASE_REF;
# JS_FRONTEND_DIFF_FORCE=1 runs anyway. Outputs stay in build/js-frontend-diff/.
set -eu

root=$(git rev-parse --show-toplevel)
cd "$root"

if [ $# -ge 1 ] && [ -n "$1" ]; then
    base_ref=$1
else
    base_ref=$(git merge-base HEAD origin/main) || {
        echo "js_frontend_diff: no merge base with origin/main (fetch it, or pass a BASE_REF)" >&2
        exit 2
    }
fi
if ! git rev-parse -q --verify "$base_ref^{commit}" >/dev/null; then
    # a shallow CI checkout may not hold the base yet
    git fetch -q --depth=1 origin "$base_ref" || true
fi
base=$(git rev-parse --verify "$base_ref^{commit}")

# What can change JS-mode output: the frontend modules, the C session, QuickJS.
inputs_of_output="stdlib/cli/js/hull/source src/hull/frontend include/hull/frontend vendor/quickjs"
# shellcheck disable=SC2086
if [ "${JS_FRONTEND_DIFF_FORCE:-0}" != 1 ] && git diff --quiet "$base" -- $inputs_of_output; then
    echo "js_frontend_diff: no frontend change since $(git rev-parse --short "$base"); nothing to compare"
    exit 0
fi

work=build/js-frontend-diff
rm -rf "$work"
mkdir -p "$work"
basetree="$root/$work/base-tree"
# shellcheck disable=SC2329  # invoked by the trap below
cleanup() { git worktree remove --force "$basetree" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

echo "js_frontend_diff: building the dump tool at HEAD"
make -s -j"$jobs" build/js_frontend_dump >"$work/build-head.log" 2>&1 || {
    tail -40 "$work/build-head.log" >&2; exit 2; }

echo "js_frontend_diff: building the dump tool at $(git rev-parse --short "$base")"
git worktree add --detach "$basetree" "$base" >/dev/null 2>&1
# The build graph wants the pinned submodules even for this small target; the worktree
# shares this repository's module store, so this normally fetches nothing.
git -C "$basetree" submodule update --init --recursive >"$work/submodules.log" 2>&1 || {
    tail -20 "$work/submodules.log" >&2; exit 2; }
# The tool is this checkout's; the frontend it links is the base's.
for f in tests/hull/frontend/js_frontend_dump.c tests/hull/frontend/js_frontend_dump.h mk/js_frontend_dump.mk; do
    cp "$f" "$basetree/$f"
done
if grep -q '^include mk/js_frontend_dump.mk' "$basetree/Makefile"; then
    extra=""
else
    extra="-f Makefile -f mk/js_frontend_dump.mk"
fi
# shellcheck disable=SC2086
(cd "$basetree" && make -s -j"$jobs" $extra build/js_frontend_dump) >"$work/build-base.log" 2>&1 || {
    tail -40 "$work/build-base.log" >&2; exit 2; }

# The live repo JS (the conformance corpus roots), the frozen inputs, and the frontend's
# own modules - all read from THIS checkout by both tools.
set -- stdlib/js/hull examples tests/fixtures stdlib/cli/js
echo "js_frontend_diff: dumping $* with both tools"
"$basetree/build/js_frontend_dump" --time --out "$work/base" "$@" 2>"$work/time-base.txt"
build/js_frontend_dump --time --out "$work/head" "$@" 2>"$work/time-head.txt"
echo "  base: $(cat "$work/time-base.txt")"
echo "  head: $(cat "$work/time-head.txt")"

files=$(find "$work/head" -name '*.parse.json' | wc -l | tr -d ' ')
if diff -r "$work/base" "$work/head" >"$work/diff.txt" 2>&1; then
    echo "js_frontend_diff: OK - JS-mode output identical for $files files"
    exit 0
fi
echo "js_frontend_diff: JS-mode output CHANGED:" >&2
grep -E '^(diff|Only in)' "$work/diff.txt" | sed -e "s#$work/##g" | head -50 >&2
echo "js_frontend_diff: full diff in $work/diff.txt" >&2
exit 1
