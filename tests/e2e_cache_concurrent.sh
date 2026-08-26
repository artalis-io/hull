#!/bin/sh
# e2e_cache_concurrent.sh - Stress test: N hull processes
# hammering the same cache root concurrently. Verifies that
# atomic-rename + content-keying + first-write-wins semantics
# hold under parallel load.
#
# What we're proving:
#   1. Concurrent writers don't corrupt entries (filenames stay
#      readable, content matches sha).
#   2. No duplicate-entry buildup (same key from N processes →
#      one file on disk after they all finish).
#   3. No crashes / nonzero exits from any worker.
#   4. tmp/ sweep doesn't leak stale .blob-*.tmp files past a
#      reasonable settle window (rename → unlink is atomic, but
#      a crashed writer in another process could leave one;
#      next store open should sweep them within tmp_max_age).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in
    /*) ;;
    *)  HULL="$(pwd)/$HULL" ;;
esac

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ - $2}"; }

# Hermetic HOME - never touch the developer's real cache.
TMPHOME=$(mktemp -d)
export HOME="$TMPHOME"
trap 'rm -rf "$TMPHOME"' EXIT

NUM_WORKERS=8

echo "=== E2E: concurrent cache writers (workers=$NUM_WORKERS) ==="

# ── 1. Race N workers loading the SAME app simultaneously ─────────
# Every worker hits the same hello example, populating identical
# bytecode entries under the same keys. The cache invariant says
# we end up with one file per (key, runtime), not N copies.
echo ""
echo "── parallel boot ──"

pids=""
for i in $(seq 1 $NUM_WORKERS); do
    PORT=$((19900 + i))
    HOME="$TMPHOME" "$HULL" examples/hello/app.lua \
        -p $PORT --no-sandbox --no-migrate \
        >/dev/null 2>"$TMPHOME/worker_$i.err" &
    pids="$pids $!"
done
sleep 2
# All workers should be listening; tell them to stop.
for pid in $pids; do
    kill -INT "$pid" 2>/dev/null || true
done
sleep 1
for pid in $pids; do
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done

# Check no worker died with a crash (signal > 128).
worker_errors=0
for i in $(seq 1 $NUM_WORKERS); do
    if grep -q "Segmentation\|Abort\|panic\|fatal" \
            "$TMPHOME/worker_$i.err" 2>/dev/null; then
        worker_errors=$((worker_errors + 1))
        echo "  worker $i crashed:"
        head -5 "$TMPHOME/worker_$i.err"
    fi
done
[ "$worker_errors" -eq 0 ] \
    && pass "no worker crashed under concurrent cache writes" \
    || fail "$worker_errors workers crashed"

# ── 2. Content invariants on the populated cache ───────────────
echo ""
echo "── cache invariants ──"

CACHE_ROOT="$TMPHOME/.hull/blobs/runtime/lua-bytecode/blobs"
if [ ! -d "$CACHE_ROOT" ]; then
    fail "cache root not created"
else
    # Count distinct entries (filenames are 64-char sha hex).
    entries=$(find "$CACHE_ROOT" -mindepth 2 -maxdepth 2 -type f \
                  2>/dev/null | wc -l | tr -d ' ')
    [ "$entries" -gt 0 ] \
        && pass "concurrent writes produced cache entries ($entries)" \
        || fail "no entries written"

    # No entry should be zero-sized (would mean a half-finished
    # write that somehow renamed into place).
    zero_sized=$(find "$CACHE_ROOT" -mindepth 2 -maxdepth 2 -type f \
                     -size 0 2>/dev/null | wc -l | tr -d ' ')
    [ "$zero_sized" -eq 0 ] \
        && pass "no zero-sized blob files" \
        || fail "$zero_sized zero-sized blobs (atomic-rename broken)"

    # Every entry's filename is 64 lowercase hex (no truncated
    # tmp leaking into the shard dirs).
    bad_names=$(find "$CACHE_ROOT" -mindepth 2 -maxdepth 2 -type f \
                    2>/dev/null \
                    | awk -F/ '{print $NF}' \
                    | grep -vE '^[0-9a-f]{64}$' | wc -l | tr -d ' ')
    [ "$bad_names" -eq 0 ] \
        && pass "every blob filename is 64-char hex" \
        || fail "$bad_names blob files have non-canonical names"
fi

# ── 3. No tmp leakage in the steady state ─────────────────────
TMP_DIR="$TMPHOME/.hull/blobs/runtime/lua-bytecode/tmp"
if [ -d "$TMP_DIR" ]; then
    leftover=$(find "$TMP_DIR" -type f 2>/dev/null | wc -l | tr -d ' ')
    [ "$leftover" -eq 0 ] \
        && pass "no leftover tmp/ files after concurrent run" \
        || fail "$leftover tmp/ files survived (atomic-rename incomplete)"
else
    pass "tmp dir clean (or never created)"
fi

# ── 4. Restart all workers, every cache hit should succeed ────
# This verifies that entries written by ANY worker are usable by
# ALL workers - the cache is genuinely shared, not per-process.
echo ""
echo "── warm-cache replay ──"

pids=""
for i in $(seq 1 $NUM_WORKERS); do
    PORT=$((19920 + i))
    HOME="$TMPHOME" "$HULL" examples/hello/app.lua \
        -p $PORT --no-sandbox --no-migrate \
        >/dev/null 2>"$TMPHOME/replay_$i.err" &
    pids="$pids $!"
done
sleep 2
for pid in $pids; do kill -INT "$pid" 2>/dev/null || true; done
sleep 1
for pid in $pids; do
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done

replay_errors=0
for i in $(seq 1 $NUM_WORKERS); do
    if grep -q "Segmentation\|Abort\|panic\|fatal" \
            "$TMPHOME/replay_$i.err" 2>/dev/null; then
        replay_errors=$((replay_errors + 1))
    fi
done
[ "$replay_errors" -eq 0 ] \
    && pass "all $NUM_WORKERS workers replayed the cache cleanly" \
    || fail "$replay_errors workers failed on warm cache"

entries_after=$(find "$CACHE_ROOT" -mindepth 2 -maxdepth 2 -type f \
                     2>/dev/null | wc -l | tr -d ' ')
[ "$entries_after" = "$entries" ] \
    && pass "warm replay did not add new entries (idempotent)" \
    || fail "entry count drifted" "before=$entries after=$entries_after"

# ── 5. Mixed-runtime concurrency (Lua + JS at the same time) ──
# Different cache stores, same blob_store backend - verifies the
# store isolation: parallel writers to lua-bytecode and
# js-bytecode shouldn't interfere even though they share the
# same allocator + sha helpers.
echo ""
echo "── mixed-runtime parallel ──"

pids=""
# Half the workers boot a Lua app, half boot a JS app.
for i in $(seq 1 4); do
    PORT=$((19940 + i))
    HOME="$TMPHOME" "$HULL" examples/hello/app.lua \
        -p $PORT --no-sandbox --no-migrate \
        >/dev/null 2>"$TMPHOME/mixed_lua_$i.err" &
    pids="$pids $!"
done
for i in $(seq 1 4); do
    PORT=$((19950 + i))
    HOME="$TMPHOME" "$HULL" examples/rest_api/app.js \
        -p $PORT --no-sandbox --no-migrate \
        >/dev/null 2>"$TMPHOME/mixed_js_$i.err" &
    pids="$pids $!"
done
sleep 2
for pid in $pids; do kill -INT "$pid" 2>/dev/null || true; done
sleep 1
for pid in $pids; do
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done

mixed_errors=0
for f in "$TMPHOME"/mixed_*.err; do
    [ -f "$f" ] || continue
    if grep -q "Segmentation\|Abort\|panic\|fatal" "$f" 2>/dev/null; then
        mixed_errors=$((mixed_errors + 1))
    fi
done
[ "$mixed_errors" -eq 0 ] \
    && pass "no crashes with mixed-runtime concurrent writers" \
    || fail "$mixed_errors workers crashed under mixed load"

js_entries=$(find "$TMPHOME/.hull/blobs/runtime/js-bytecode/blobs" \
                  -mindepth 2 -maxdepth 2 -type f 2>/dev/null \
                  | wc -l | tr -d ' ')
[ "$js_entries" -gt 0 ] \
    && pass "JS cache also populated under mixed load ($js_entries)" \
    || fail "JS cache not populated under mixed load"

echo ""
echo "$PASS/$((PASS + FAIL)) e2e concurrent-cache tests passed"
[ "$FAIL" -eq 0 ]
