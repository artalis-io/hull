#!/bin/sh
# e2e_aot_cache.sh — End-to-end tests for the AOT artifact cache
# (~/.hull/blobs/runtime/compute-aot/) wired into `hull build`.
#
# Exercises the cold/warm/opt-out paths against examples/compute
# which has 8 WASM modules. Requires `wamrc` — skips cleanly when
# absent.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in
    /*) ;;
    *)  HULL="$(pwd)/$HULL" ;;
esac
WAMRC="${WAMRC:-build/wamrc}"
case "$WAMRC" in
    /*) ;;
    *)  WAMRC="$(pwd)/$WAMRC" ;;
esac

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ — $2}"; }

if [ ! -x "$WAMRC" ]; then
    echo "  SKIP: wamrc not found at $WAMRC (build with: make wamrc)"
    exit 0
fi

# Isolate the cache root so the test never reads/writes the
# developer's real $HOME/.hull/blobs/. The runtime resolves the
# cache via $HOME — overriding it is all we need.
TMPHOME=$(mktemp -d)
export HOME="$TMPHOME"
trap 'rm -rf "$TMPHOME"' EXIT

# Runtime caches live under $HOME/.hull/blobs/runtime/<kind>/blobs/
# <XX>/<sha256> (the keyed-blob_store sharded layout). Count by
# walking the shard tree.
CACHE_ROOT="$HOME/.hull/blobs/runtime/compute-aot/blobs"
OUT=$(mktemp -d)
APP_DIR="examples/compute"

# Wamrc resolution uses find_tool which walks $HOME/.hull/tools first;
# put a symlink there so wamrc is found even with the redirected HOME.
mkdir -p "$HOME/.hull/tools"
ln -s "$WAMRC" "$HOME/.hull/tools/wamrc"

# Count cached entries by walking the two-level shard tree.
# Layout: $CACHE_ROOT/<XX>/<sha256-hex>.
count_cache() {
    if [ ! -d "$CACHE_ROOT" ]; then echo 0; return; fi
    find "$CACHE_ROOT" -mindepth 2 -maxdepth 2 -type f 2>/dev/null \
        | wc -l | tr -d ' '
}

# Wipe the whole cache subtree (parent of the shard dirs).
wipe_cache() {
    rm -rf "$HOME/.hull/blobs/runtime/compute-aot"
}

echo "=== E2E: AOT cache (hull build) ==="

# ── 1. Cold build populates cache ────────────────────────────────
wipe_cache
OUT1=$("$HULL" build "$APP_DIR" --output "$OUT/app1" --runtime lua \
            --no-verify-platform 2>&1) || true
N1=$(count_cache)
[ "$N1" = "8" ] && pass "cold build populated cache (8 entries)" \
                || fail "cold build cache count" "got $N1, want 8"
echo "$OUT1" | grep -q "8 AOT module(s) compiled, 8 written to cache" \
    && pass "cold build reports 8 written to cache" \
    || fail "cold build summary" "expected '8 written to cache' in output"
echo "$OUT1" | grep -q "\[cache hit\]" \
    && fail "cold build should not report cache hits" \
    || pass "cold build reports no cache hits"

# ── 2. Warm build hits cache for every module ────────────────────
OUT2=$("$HULL" build "$APP_DIR" --output "$OUT/app2" --runtime lua \
            --no-verify-platform 2>&1) || true
N2=$(count_cache)
[ "$N2" = "8" ] && pass "warm build leaves cache count unchanged" \
                || fail "warm cache count" "got $N2, want 8"
HITS=$(echo "$OUT2" | grep -c "\[cache hit\]" || true)
[ "$HITS" = "8" ] && pass "warm build reports 8 cache hits" \
                  || fail "warm cache hit count" "got $HITS, want 8"
echo "$OUT2" | grep -q "8 AOT module(s) compiled (8 from cache)" \
    && pass "warm build summary lists cache reuse" \
    || fail "warm build summary"

# ── 3. HULL_NO_AOT_CACHE bypasses the cache ──────────────────────
wipe_cache
OUT3=$(HULL_NO_AOT_CACHE=1 "$HULL" build "$APP_DIR" --output "$OUT/app3" \
            --runtime lua --no-verify-platform 2>&1) || true
N3=$(count_cache)
[ "$N3" = "0" ] && pass "HULL_NO_AOT_CACHE skipped cache writes" \
                || fail "no-aot-cache write count" "got $N3, want 0"
echo "$OUT3" | grep -q "\[cache hit\]" \
    && fail "HULL_NO_AOT_CACHE should disable lookups too" \
    || pass "HULL_NO_AOT_CACHE skipped cache lookups"

# ── 4. --no-cache CLI flag bypasses the cache ────────────────────
wipe_cache
OUT4=$("$HULL" build "$APP_DIR" --output "$OUT/app4" --runtime lua \
            --no-verify-platform --no-cache 2>&1) || true
N4=$(count_cache)
[ "$N4" = "0" ] && pass "--no-cache skipped writes" \
                || fail "--no-cache write count" "got $N4, want 0"

# ── 5. HULL_NO_CACHE=1 global kill-switch ────────────────────────
wipe_cache
OUT5=$(HULL_NO_CACHE=1 "$HULL" build "$APP_DIR" --output "$OUT/app5" \
            --runtime lua --no-verify-platform 2>&1) || true
N5=$(count_cache)
[ "$N5" = "0" ] && pass "HULL_NO_CACHE skipped writes" \
                || fail "HULL_NO_CACHE write count" "got $N5, want 0"

# ── 6. Cache survives across binary outputs (content-addressed) ──
# After step 5 the cache is empty; the first build refilled it (via
# step 1 then step 2 left it at 8). Re-establish cache + verify a
# third build still hits.
wipe_cache
"$HULL" build "$APP_DIR" --output "$OUT/app6" --runtime lua \
        --no-verify-platform >/dev/null 2>&1
OUT7=$("$HULL" build "$APP_DIR" --output "$OUT/app7" --runtime lua \
            --no-verify-platform 2>&1) || true
HITS3=$(echo "$OUT7" | grep -c "\[cache hit\]" || true)
[ "$HITS3" = "8" ] && pass "cross-invocation cache reuse" \
                   || fail "cross-invocation reuse" "got $HITS3 hits, want 8"

# ── 7. Built binary still runs after cache hit ───────────────────
# The cached AOT was produced once at step 6 and replayed at step 7;
# loading it through the embedded VFS proves the cache→build→runtime
# round-trip preserves AOT correctness.
"$OUT/app7" -p 19874 --no-migrate --no-sandbox >/dev/null 2>&1 &
APP_PID=$!
sleep 1
HEALTH=$(curl -sf --max-time 2 "http://127.0.0.1:19874/health" 2>/dev/null || echo "")
kill -INT $APP_PID 2>/dev/null || true
# Give the app a moment to drain, then force-kill if still alive.
sleep 1
kill -KILL $APP_PID 2>/dev/null || true
wait $APP_PID 2>/dev/null || true
case "$HEALTH" in
    *'"ok":true'*) pass "warm-cache binary responds /health" ;;
    *)             fail "warm-cache binary /health" "got: $HEALTH" ;;
esac

rm -rf "$OUT"

echo ""
echo "$PASS/$((PASS + FAIL)) e2e AOT cache tests passed"
[ "$FAIL" -eq 0 ]
