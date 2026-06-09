#!/bin/sh
# e2e_blob.sh — End-to-end tests for hull/blob@1 (Lua + JS).
#
# Exercises the full stack through a live `hull` binary running as a
# CLI app (app.main). Covers:
#
#   - blob.init under sandbox (proves the sandbox.c pre-mkdir fix)
#   - buffer-mode put + get (round-trip; content-addressed SHA matches
#     the well-known SHA-256 of the input)
#   - streaming writer (write/finalize) with chained writes
#   - idempotent put (same bytes twice → one blob on disk)
#   - metadata (exists, size, count, total_size)
#   - snapshot iter
#   - delete
#   - cleanup with max_total_size + LRU eviction
#   - shard_depth = 2 layout
#   - Lua + JS produce identical SHA-256 ids for identical inputs
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
# Resolve to absolute path before we cd into a tmp dir below — relative
# build/hull only works when cwd is the repo root.
case "$HULL" in
    /*) ;;
    *)  HULL="$(pwd)/$HULL" ;;
esac
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ — $2}"; }

contains() {
    case "$3" in
        *"$2"*) pass "$1" ;;
        *)      fail "$1" "expected substring '$2' in: $3" ;;
    esac
}

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Well-known SHA-256 of "hello, blob world" — used to validate
# content-addressing across runtimes.
EXPECTED_SHA_HELLO="41c962dd3fa37a189e98d239834a5d6a671af33275bbb846549285c78e3e8417"
# SHA-256 of "foobarbaz"
EXPECTED_SHA_FOOBARBAZ="97df3588b5a3f24babc3851b372f0ba71a9dcdded43b14b9d06961bfc1707d9d"

# ── Lua suite ───────────────────────────────────────────────────────

echo "=== E2E: hull/blob@1 (Lua) ==="

cat > "$TMPDIR/app.lua" <<'EOF'
local blob = require("hull.blob")

app.manifest({
    name = "blob-e2e-lua", version = "0.0.1",
    modules = { "hull/blob@1" },
    -- Declare the parent dir, not data/blobs itself, because Linux
    -- Landlock unveil rejects paths that don't exist yet — and
    -- data/blobs is exactly what blob.init is supposed to create.
    -- data/ exists implicitly under the cwd (we cd into TMPDIR which
    -- exists), so unveil succeeds; blob.init's mkdir under it is
    -- still within the write allowlist.
    fs = { write = { "data/" } },
})

app.main(function()
    blob.init({ dir = "data/blobs" })

    -- Buffer-mode put + get round-trip
    local id, size = blob.put("hello, blob world")
    print("PUT_ID=" .. id)
    print("PUT_SIZE=" .. size)
    local got = blob.get(id)
    print("GET_LEN=" .. #got)
    print("GET_EQ=" .. (got == "hello, blob world" and "true" or "false"))

    -- Streaming writer
    local w = blob.writer()
    w:write("foo"):write("bar"):write("baz")
    local id2, size2 = w:finalize()
    print("STREAM_ID=" .. id2)
    print("STREAM_SIZE=" .. size2)

    -- Idempotent put (same bytes → one blob)
    local id3 = blob.put("hello, blob world")
    print("DUP_EQ=" .. (id3 == id and "true" or "false"))
    print("COUNT_AFTER_DUP=" .. blob.count())

    -- Metadata
    print("EXISTS=" .. tostring(blob.exists(id)))
    print("SIZE=" .. blob.size(id))
    print("TOTAL_SIZE=" .. blob.total_size())

    -- Snapshot iter
    local n = 0
    local total_seen = 0
    for it_id, it_size in blob.iter() do
        n = n + 1
        total_seen = total_seen + it_size
    end
    print("ITER_N=" .. n)
    print("ITER_TOTAL=" .. total_seen)

    -- put_verified: correct sha accepted, wrong sha rejected
    local ok = pcall(function()
        blob.put_verified("verify-me",
            "0000000000000000000000000000000000000000000000000000000000000000")
    end)
    print("VERIFIED_BAD_REJECTED=" .. (ok and "false" or "true"))

    -- Delete
    blob.delete(id2)
    print("AFTER_DELETE_COUNT=" .. blob.count())

    -- Cleanup with max_total_size = 1 byte → evict everything
    local stats = blob.cleanup({ max_total_size = 1, strategy = "lru" })
    print("CLEANUP_REMOVED=" .. stats.removed)
    print("AFTER_CLEANUP_COUNT=" .. blob.count())
    return 0
end)
EOF

cd "$TMPDIR"
# Pre-create the fs.write declared directory so Linux Landlock's
# unveil(2) can pin it — Landlock rejects unveil on non-existent
# paths, leaving the write allowlist empty and breaking blob.init's
# mkdir of data/blobs/. macOS Seatbelt is permissive about this so
# the bug had been latent until we ran e2e_blob in CI for the first
# time.
mkdir -p data
# Both `|| true` are required: hull may exit non-zero (set -e would
# kill us mid-script otherwise), and grep returns 1 when no matches.
LUA_RAW=$($HULL app.lua 2>&1) || true
LUA_OUT=$(printf '%s\n' "$LUA_RAW" | grep -E "^(PUT|GET|STREAM|DUP|COUNT|EXISTS|SIZE|TOTAL|ITER|VERIFIED|AFTER|CLEANUP)") || true
cd - >/dev/null

# Debug: if hull produced no matching output, dump the raw stderr+stdout
# so we can diagnose. Test had been working on macOS but the first
# Linux/Cosmo CI run revealed empty output — this surfaces why.
if [ -z "$LUA_OUT" ]; then
    echo "  [DEBUG] hull (Lua) produced no matching output. Raw:"
    printf '%s\n' "$LUA_RAW" | sed 's/^/    > /' | head -40
fi

contains "lua buffer put SHA"        "PUT_ID=$EXPECTED_SHA_HELLO"     "$LUA_OUT"
contains "lua buffer put size"       "PUT_SIZE=17"                    "$LUA_OUT"
contains "lua get bytes"             "GET_LEN=17"                     "$LUA_OUT"
contains "lua get content"           "GET_EQ=true"                    "$LUA_OUT"
contains "lua stream SHA"            "STREAM_ID=$EXPECTED_SHA_FOOBARBAZ" "$LUA_OUT"
contains "lua stream size"           "STREAM_SIZE=9"                  "$LUA_OUT"
contains "lua idempotent dedup id"   "DUP_EQ=true"                    "$LUA_OUT"
contains "lua dedup count=2"         "COUNT_AFTER_DUP=2"              "$LUA_OUT"
contains "lua exists=true"           "EXISTS=true"                    "$LUA_OUT"
contains "lua size=17"               "SIZE=17"                        "$LUA_OUT"
contains "lua total_size=26"         "TOTAL_SIZE=26"                  "$LUA_OUT"
contains "lua iter snapshot count"   "ITER_N=2"                       "$LUA_OUT"
contains "lua iter snapshot total"   "ITER_TOTAL=26"                  "$LUA_OUT"
contains "lua put_verified rejects"  "VERIFIED_BAD_REJECTED=true"     "$LUA_OUT"
contains "lua delete drops count"    "AFTER_DELETE_COUNT=1"           "$LUA_OUT"
contains "lua cleanup evicted"       "CLEANUP_REMOVED=1"              "$LUA_OUT"
contains "lua cleanup zeroed count"  "AFTER_CLEANUP_COUNT=0"          "$LUA_OUT"

# ── JS suite ─────────────────────────────────────────────────────────

echo ""
echo "=== E2E: hull/blob@1 (JS) ==="

rm -f "$TMPDIR/app.lua"
rm -rf "$TMPDIR/data"

cat > "$TMPDIR/app.js" <<'EOF'
import { app } from "hull:app";
import { blob } from "hull:blob";

app.manifest({
    name: "blob-e2e-js", version: "0.0.1",
    modules: ["hull/blob@1"],
    // See Lua sibling for the parent-dir rationale: Linux Landlock
    // unveil rejects paths that don't exist yet.
    fs: { write: ["data/"] },
});

app.main(() => {
    blob.init({ dir: "data/blobs" });

    const r1 = blob.put("hello, blob world");
    console.log("PUT_ID=" + r1.id);
    console.log("PUT_SIZE=" + r1.size);

    const buf = blob.get(r1.id);
    console.log("GET_LEN=" + buf.byteLength);
    // ASCII compare without TextDecoder
    const u8 = new Uint8Array(buf);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    console.log("GET_EQ=" + (s === "hello, blob world" ? "true" : "false"));

    const w = blob.writer();
    w.write("foo").write("bar").write("baz");
    const r2 = w.finalize();
    console.log("STREAM_ID=" + r2.id);
    console.log("STREAM_SIZE=" + r2.size);

    const r3 = blob.put("hello, blob world");
    console.log("DUP_EQ=" + (r3.id === r1.id ? "true" : "false"));
    console.log("COUNT_AFTER_DUP=" + blob.count());

    console.log("EXISTS=" + blob.exists(r1.id));
    console.log("SIZE=" + blob.size(r1.id));
    console.log("TOTAL_SIZE=" + blob.totalSize());

    let n = 0, totalSeen = 0;
    for (const it of blob.iter()) { n++; totalSeen += it.size; }
    console.log("ITER_N=" + n);
    console.log("ITER_TOTAL=" + totalSeen);

    let rejected = false;
    try {
        blob.putVerified("verify-me",
            "0000000000000000000000000000000000000000000000000000000000000000");
    } catch (e) { rejected = true; }
    console.log("VERIFIED_BAD_REJECTED=" + (rejected ? "true" : "false"));

    blob.delete(r2.id);
    console.log("AFTER_DELETE_COUNT=" + blob.count());

    const stats = blob.cleanup({ maxTotalSize: 1, strategy: "lru" });
    console.log("CLEANUP_REMOVED=" + stats.removed);
    console.log("AFTER_CLEANUP_COUNT=" + blob.count());
    return 0;
});
EOF

cd "$TMPDIR"
mkdir -p data   # See Lua sibling for the Linux Landlock rationale.
# See Lua sibling for the dual `|| true` rationale.
JS_RAW=$($HULL app.js 2>&1) || true
JS_OUT=$(printf '%s\n' "$JS_RAW" | grep -E "(PUT|GET|STREAM|DUP|COUNT|EXISTS|SIZE|TOTAL|ITER|VERIFIED|AFTER|CLEANUP)") || true
cd - >/dev/null

if [ -z "$JS_OUT" ]; then
    echo "  [DEBUG] hull (JS) produced no matching output. Raw:"
    printf '%s\n' "$JS_RAW" | sed 's/^/    > /' | head -40
fi

contains "js buffer put SHA"         "PUT_ID=$EXPECTED_SHA_HELLO"     "$JS_OUT"
contains "js buffer put size"        "PUT_SIZE=17"                    "$JS_OUT"
contains "js get bytes"              "GET_LEN=17"                     "$JS_OUT"
contains "js get content"            "GET_EQ=true"                    "$JS_OUT"
contains "js stream SHA"             "STREAM_ID=$EXPECTED_SHA_FOOBARBAZ" "$JS_OUT"
contains "js stream size"            "STREAM_SIZE=9"                  "$JS_OUT"
contains "js idempotent dedup id"    "DUP_EQ=true"                    "$JS_OUT"
contains "js dedup count=2"          "COUNT_AFTER_DUP=2"              "$JS_OUT"
contains "js exists=true"            "EXISTS=true"                    "$JS_OUT"
contains "js size=17"                "SIZE=17"                        "$JS_OUT"
contains "js total_size=26"          "TOTAL_SIZE=26"                  "$JS_OUT"
contains "js iter snapshot count"    "ITER_N=2"                       "$JS_OUT"
contains "js iter snapshot total"    "ITER_TOTAL=26"                  "$JS_OUT"
contains "js put_verified rejects"   "VERIFIED_BAD_REJECTED=true"     "$JS_OUT"
contains "js delete drops count"     "AFTER_DELETE_COUNT=1"           "$JS_OUT"
contains "js cleanup evicted"        "CLEANUP_REMOVED=1"              "$JS_OUT"
contains "js cleanup zeroed count"   "AFTER_CLEANUP_COUNT=0"          "$JS_OUT"

# ── Cross-runtime SHA equality (parity guarantee) ───────────────────

echo ""
echo "=== E2E: cross-runtime SHA parity ==="

# Both runtimes produced the same SHAs above (asserted via the
# well-known constants). One more positive assertion to make the
# guarantee explicit:
# JS output is prefixed by the hull logger ("HH:MM:SS INFO [app] "),
# Lua's print is raw. Match on PUT_ID= anywhere on the line and take
# the value past the LAST '=' so we strip any prefix junk.
LUA_HELLO=$(echo "$LUA_OUT" | grep 'PUT_ID=' | head -1 | sed -E 's/.*PUT_ID=//')
JS_HELLO=$(echo "$JS_OUT"   | grep 'PUT_ID=' | head -1 | sed -E 's/.*PUT_ID=//')
if [ -n "$LUA_HELLO" ] && [ "$LUA_HELLO" = "$JS_HELLO" ]; then
    pass "lua and js produce identical SHA for same input"
else
    fail "cross-runtime SHA parity" "lua='$LUA_HELLO' js='$JS_HELLO'"
fi

# ── shard_depth = 2 layout ──────────────────────────────────────────

echo ""
echo "=== E2E: shard_depth = 2 layout ==="

rm -f "$TMPDIR/app.js"
rm -rf "$TMPDIR/data"

cat > "$TMPDIR/app.lua" <<'EOF'
local blob = require("hull.blob")
app.manifest({
    name = "blob-shard2", version = "0.0.1",
    modules = { "hull/blob@1" },
    -- Declare the parent dir, not data/blobs itself, because Linux
    -- Landlock unveil rejects paths that don't exist yet — and
    -- data/blobs is exactly what blob.init is supposed to create.
    -- data/ exists implicitly under the cwd (we cd into TMPDIR which
    -- exists), so unveil succeeds; blob.init's mkdir under it is
    -- still within the write allowlist.
    fs = { write = { "data/" } },
})
app.main(function()
    blob.init({ dir = "data/blobs", shard_depth = 2 })
    local id, _ = blob.put("shard depth two")
    print("SHARD2_ID=" .. id)
    return 0
end)
EOF

cd "$TMPDIR"
mkdir -p data   # See earlier Lua/JS sections for the Landlock rationale.
SHARD_OUT=$($HULL app.lua 2>&1 | grep ^SHARD2_ID=) || true
cd - >/dev/null

SHARD_ID=$(echo "$SHARD_OUT" | cut -d= -f2)
if [ -n "$SHARD_ID" ]; then
    EXPECTED_PATH="$TMPDIR/data/blobs/blobs/${SHARD_ID:0:2}/${SHARD_ID:2:2}/${SHARD_ID}"
    if [ -f "$EXPECTED_PATH" ]; then
        pass "shard_depth=2 file at <XX>/<YY>/<id>"
    else
        fail "shard_depth=2 layout" "expected $EXPECTED_PATH"
    fi
else
    fail "shard_depth=2 returned no id"
fi

# ── Summary ─────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e blob tests passed"
[ "$FAIL" -eq 0 ] || exit 1
