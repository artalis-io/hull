#!/bin/sh
# Hull ext: deterministic carriage for the out-of-tree WAMR patches.
#
# Applies patches/wamr/0001 + 0002 + 0003 + 0004 onto a CLEAN checkout of the pinned base into
# an isolated staged tree that Hull builds against (WAMR_DIR=build/wamr-patched).
# vendor/wamr is NEVER mutated. The step fails loudly on:
#   - vendor/wamr not at the pinned base commit, or dirty      (verify-base)
#   - a patch file whose sha-256 drifted from the recorded pin (stale)
#   - a patch that does not apply with exact context           (offset/stale)
#   - the applied tree not reverting cleanly to the base       (tamper)
#   - any changed/new file NOT declared by the patches         (unexpected source)
#
# Usage:
#   scripts/wamr_apply_patches.sh            # apply into build/wamr-patched
#   scripts/wamr_apply_patches.sh --dry-run  # CI gate: validate only, no output tree kept
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BASE=c3a78cd159e59c86ac4543308bd676ff78d30a93
PATCHDIR=patches/wamr
P1=$PATCHDIR/0001-tests-unit-wasi-sdk-dir-overridable.patch
P2=$PATCHDIR/0002-shared-heap-readonly-permission.patch
P3=$PATCHDIR/0003-shared-heap-destroy.patch
P4=$PATCHDIR/0004-shared-heap-guarded-subrange.patch
SHA1=423beeae0e94454381ce0d805e9985c5cd94e14e511981c629af186676411698
SHA2=310706eb6a36ae33756c85997b6599b4855d279e350ba7dae7c5b0353a6c8177
SHA3=e8f3362cfef0dccc975c3e687390429f06a0d0c63d73f3959be7a5c36f1f1d2c
SHA4=e5bf6e04b89d878745198421ad9cbf94ac567edf07297887354b35d98b7f4dbd

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

fail() { echo "FATAL: $*" >&2; exit 1; }
# Prefer sha256sum (Linux / Alpine busybox) then shasum (macOS has no sha256sum).
# NOT `shasum ... | awk || sha256sum ...`: a missing shasum makes the pipe's exit
# status awk's (0 on empty input), so the `||` fallback never fires and the hash
# comes back empty -- which silently fails the recorded-hash check. Detect the
# tool up front instead.
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        fail "no sha256sum or shasum available to hash $1"
    fi
}

# One cleanup for temp files, the dry-run tree, and the apply lock. The lock is
# removed ONLY when we own it (LOCK non-empty), never another process's.
LOCK=""; CLEAN_TMP=""
cleanup() {
    [ -n "$CLEAN_TMP" ] && rm -f $CLEAN_TMP 2>/dev/null || :
    { [ "$DRY_RUN" -eq 1 ] && [ -n "${STAGED:-}" ] && rm -rf "$STAGED" 2>/dev/null; } || :
    [ -n "$LOCK" ] && rmdir "$LOCK" 2>/dev/null || :
    return 0
}
trap cleanup EXIT INT TERM

if [ "$DRY_RUN" -eq 1 ]; then
    STAGED=$(mktemp -d "${TMPDIR:-/tmp}/wamr-patched.XXXXXX")
else
    # Destination staged tree. The Makefile passes WAMR_STAGE_DIR=$(WAMR_DIR) so a
    # BUILDDIR override (build/tlsless, build/keelless, a sub-make, ...) stages
    # into its OWN $(BUILDDIR)/wamr-patched -- matching the stamp path the Makefile
    # then touches. Default keeps the plain build/wamr-patched for a bare run.
    STAGED="${WAMR_STAGE_DIR:-build/wamr-patched}"
    case "$STAGED" in /*) ;; *) STAGED="$ROOT/$STAGED" ;; esac
    STAGE_PARENT=$(dirname "$STAGED")
    # Serialise concurrent applies to the SAME staged tree (mkdir is atomic, so
    # only one process re-creates it at a time). The lock sits beside the tree, so
    # distinct BUILDDIRs get distinct locks and never serialise against each other.
    mkdir -p "$STAGE_PARENT"
    _lockdir="$STAGE_PARENT/.wamr-apply.lock"
    _t=0
    while ! mkdir "$_lockdir" 2>/dev/null; do
        _t=$((_t + 1))
        [ "$_t" -gt 600 ] && fail "timed out (60s) waiting for the WAMR apply lock $_lockdir"
        sleep 0.1
    done
    LOCK="$_lockdir"   # we now own it; cleanup will release it
fi

# --- verify-base / choose the base source -------------------------------------
# Preferred: the git submodule -- full verify-base (HEAD == pinned) + clean-tree
# check + `git archive`. Fallback: the vendor/wamr WORKING TREE, for environments
# where the submodule git-dir is unavailable (a repo COPIED into a container
# without a usable .git/modules -- the Alpine musl builders in tests/e2e_musl.sh,
# scripts/build_musl_platform.sh, and release.yml do exactly this). The working
# tree equals `git archive HEAD` when the submodule is clean, which the host
# CI/release gate (`make wamr-patch-check`, run on every push) verifies with full
# git integrity. In BOTH modes the patch-hash, apply, reverse-check, and
# unexpected-source audits below still run; only the git-based HEAD==pinned check
# is skipped in worktree mode.
have=$(git -C vendor/wamr rev-parse HEAD 2>/dev/null || echo none)
if [ "$have" = "$BASE" ]; then
    [ -z "$(git -C vendor/wamr status --porcelain 2>/dev/null)" ] \
        || fail "vendor/wamr working tree is dirty; refusing to stage from it"
    SRC_MODE=git
elif [ -f vendor/wamr/core/config.h ] && [ -d vendor/wamr/core/iwasm ]; then
    SRC_MODE=worktree
    echo "note: vendor/wamr git metadata unavailable (have=$have); staging from the" \
         "working tree. git-based verify-base is skipped; patch-hash + apply +" \
         "reverse-check + unexpected-source audits still enforced. Run" \
         "'make wamr-patch-check' on a git host for the strict gate." >&2
else
    fail "vendor/wamr at $have, expected pinned base $BASE, and no usable working tree"
fi

# Emit the pristine base tree into $1: `git archive` when the submodule is usable,
# otherwise a tar of the working tree (excluding its .git gitlink/dir). Used for
# both the staged copy and the unexpected-source base reference, so the two are
# always produced the same way and the audit's diff is exactly the patch effect.
stage_base_into() {
    if [ "$SRC_MODE" = git ]; then
        git -C vendor/wamr archive "$BASE" | tar -x -C "$1"
    else
        # Copy the working tree, then drop the top-level .git gitlink from the
        # destination (portable across bsdtar/gnutar -- no --exclude pattern
        # quirks). `git -C "$STAGED" init` re-creates a fresh .git afterwards.
        ( cd vendor/wamr && tar cf - . ) | tar -x -C "$1"
        rm -rf "$1/.git"
    fi
}

# --- stale patch detection (recorded sha-256) ---------------------------------
[ -f "$P1" ] || fail "missing $P1"
[ -f "$P2" ] || fail "missing $P2"
[ -f "$P3" ] || fail "missing $P3"
[ -f "$P4" ] || fail "missing $P4"
g1=$(sha256 "$P1"); [ "$g1" = "$SHA1" ] || fail "0001 sha256 $g1 != recorded $SHA1 (stale patch)"
g2=$(sha256 "$P2"); [ "$g2" = "$SHA2" ] || fail "0002 sha256 $g2 != recorded $SHA2 (stale patch)"
g3=$(sha256 "$P3"); [ "$g3" = "$SHA3" ] || fail "0003 sha256 $g3 != recorded $SHA3 (stale patch)"
g4=$(sha256 "$P4"); [ "$g4" = "$SHA4" ] || fail "0004 sha256 $g4 != recorded $SHA4 (stale patch)"

# --- staged-copy (clean checkout of the base; vendor/wamr untouched) ----------
rm -rf "$STAGED"; mkdir -p "$STAGED"
stage_base_into "$STAGED"
# Own git dir so `git apply` operates within the staged tree and does NOT walk
# up to Hull's repo (which would apply paths against the Hull root).
git -C "$STAGED" init -q

# --- dry-run each patch (exact-context; fails on offset/stale) ----------------
git -C "$STAGED" apply --check --whitespace=nowarn "$ROOT/$P1" \
    || fail "0001 does not apply cleanly (offset/stale vs base)"
git -C "$STAGED" apply --whitespace=nowarn "$ROOT/$P1"
git -C "$STAGED" apply --check --whitespace=nowarn "$ROOT/$P2" \
    || fail "0002 does not apply cleanly (offset/stale vs base+0001)"
git -C "$STAGED" apply --whitespace=nowarn "$ROOT/$P2"
git -C "$STAGED" apply --check --whitespace=nowarn "$ROOT/$P3" \
    || fail "0003 does not apply cleanly (offset/stale vs base+0001+0002)"
git -C "$STAGED" apply --whitespace=nowarn "$ROOT/$P3"
git -C "$STAGED" apply --check --whitespace=nowarn "$ROOT/$P4" \
    || fail "0004 does not apply cleanly (offset/stale vs base+0001+0002+0003)"
git -C "$STAGED" apply --whitespace=nowarn "$ROOT/$P4"

# --- tamper check: the applied tree must revert cleanly to the TOP patch -------
# 0004 is the top of the stack (it edits wasm_memory.c which 0002/0003 also
# touch, so the reverse check must target 0004). Reversing the top patch proves
# the staged tree carries exactly 0004's content; lower patches are covered by their
# exact-context forward --check above plus the unexpected-source audit below.
git -C "$STAGED" apply --reverse --check --whitespace=nowarn "$ROOT/$P4" \
    || fail "applied tree does not reverse-match 0004 (tampered/extra content)"

# --- unexpected-source audit: changed/new file SET must equal the patch set ---
declared=$(mktemp); actual=$(mktemp)
CLEAN_TMP="$declared $actual"   # picked up by the single cleanup() trap
{ grep -E '^\+\+\+ b/' "$ROOT/$P1" "$ROOT/$P2" "$ROOT/$P3" "$ROOT/$P4"; } \
    | sed -E 's/^.*\+\+\+ b\///' | sort -u > "$declared"
# every declared file exists in the staged tree
while IFS= read -r f; do
    [ -f "$STAGED/$f" ] || fail "declared patch file missing after apply: $f"
done < "$declared"
# no source file changed outside the declared set (compare staged vs a fresh
# base with cmp, so no fragile diff-output path parsing).
BASECOPY=$(mktemp -d "${TMPDIR:-/tmp}/wamr-base.XXXXXX")
stage_base_into "$BASECOPY"
( cd "$STAGED" && find . -type f ! -path './.git/*' | sed 's#^\./##' ) \
    | while IFS= read -r f; do
        if [ ! -f "$BASECOPY/$f" ]; then
            echo "$f"                                   # new file
        elif ! cmp -s "$BASECOPY/$f" "$STAGED/$f"; then
            echo "$f"                                   # changed file
        fi
    done | sort -u > "$actual"
rm -rf "$BASECOPY"
if ! diff -q "$declared" "$actual" >/dev/null; then
    echo "FATAL: applied tree changes files not declared by the patches:" >&2
    diff "$declared" "$actual" >&2 || true
    exit 1
fi

# Drop the scratch .git so the staged tree is a plain source dir for the build.
rm -rf "$STAGED/.git"

echo "OK: pinned base $BASE + 0001 + 0002 + 0003 + 0004 -> $STAGED"
echo "     $(wc -l < "$declared" | tr -d ' ') changed/new files, all declared; reverse-check clean."
[ "$DRY_RUN" -eq 1 ] && echo "     (dry-run: staged tree discarded)"
exit 0
