#!/bin/sh
# tests/e2e_spans_mmap.sh - windowed fs.mmap({offset,length}) binding
# (mapped-spans checkpoint 3a, item A). The MappedBuffer is a zero-copy handle
# (no script-side byte access - bytes flow to compute/gpu), so this asserts the
# script-observable contract: the returned window's len() is the EOF-clamped
# length, a bare path stays whole-file, and bad args fail cleanly. Window
# byte-correctness is proven at the C level (tests/hull/cap/test_wasm_spans.c).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL_BIN="${HULL_BIN:-build/hull}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
expect_eq() {
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (expected '$2', got '$3')"; fi
}

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# A 10000-byte data file (0x2710). 10000 = 2*4096 + 1808, so a window past the
# page edge and an EOF-clamped tail are both exercised.
head -c 10000 /dev/zero > "$TMPDIR/data.bin"

# ── Lua fixture ─────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.lua" << 'EOF'
local fs = require("hull.fs")
app.manifest({ modules = { "hull/fs@1" }, fs = { read = { "data.bin" } } })
app.main(function(ctx)
    -- windowed: len == requested length when wholly inside the file
    local w = fs.mmap("data.bin", { offset = 100, length = 64 })
    ctx.stdout:write("win=" .. w:len() .. "\n"); w:close()
    -- EOF clamp: request past end -> clamped to file end (10000 - 9000 = 1000)
    local c = fs.mmap("data.bin", { offset = 9000, length = 5000 })
    ctx.stdout:write("clamp=" .. c:len() .. "\n"); c:close()
    -- offset 0 default is allowed (omit offset)
    local o = fs.mmap("data.bin", { length = 128 })
    ctx.stdout:write("noff=" .. o:len() .. "\n"); o:close()
    -- bare path -> whole file
    local whole = fs.mmap("data.bin")
    ctx.stdout:write("whole=" .. whole:len() .. "\n"); whole:close()
    -- error cases fail cleanly (pcall)
    ctx.stdout:write("nolen=" .. tostring(pcall(fs.mmap, "data.bin", {})) .. "\n")
    ctx.stdout:write("neg=" .. tostring(pcall(fs.mmap, "data.bin", { offset = -1, length = 10 })) .. "\n")
    ctx.stdout:write("zero=" .. tostring(pcall(fs.mmap, "data.bin", { length = 0 })) .. "\n")
    return 0
end)
EOF

# ── JS fixture ──────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.js" << 'EOF'
import { app } from "hull:app";
import { fs } from "hull:fs";
app.manifest({ modules: ["hull/fs@1"], fs: { read: ["data.bin"] } });
app.main((ctx) => {
    const w = fs.mmap("data.bin", { offset: 100, length: 64 });
    ctx.stdout.write("win=" + w.length + "\n"); w.close();
    const c = fs.mmap("data.bin", { offset: 9000, length: 5000 });
    ctx.stdout.write("clamp=" + c.length + "\n"); c.close();
    const o = fs.mmap("data.bin", { length: 128 });
    ctx.stdout.write("noff=" + o.length + "\n"); o.close();
    const whole = fs.mmap("data.bin");
    ctx.stdout.write("whole=" + whole.length + "\n"); whole.close();
    const tryFail = (f) => { try { f(); return "true"; } catch (e) { return "false"; } };
    ctx.stdout.write("nolen=" + tryFail(() => fs.mmap("data.bin", {})) + "\n");
    ctx.stdout.write("neg=" + tryFail(() => fs.mmap("data.bin", { offset: -1, length: 10 })) + "\n");
    ctx.stdout.write("zero=" + tryFail(() => fs.mmap("data.bin", { length: 0 })) + "\n");
    return 0;
});
EOF

run_rt() {
    runtime="$1"; ext="$2"
    echo "--- windowed fs.mmap (${runtime}) ---"
    out=$(cd "$TMPDIR" && "$OLDPWD/${HULL_BIN}" run "app.${ext}" 2>/dev/null)
    expect_eq "${runtime} window len == requested"  "win=64"    "$(echo "$out" | grep '^win=')"
    expect_eq "${runtime} window EOF-clamped"        "clamp=1000" "$(echo "$out" | grep '^clamp=')"
    expect_eq "${runtime} offset defaults to 0"      "noff=128"   "$(echo "$out" | grep '^noff=')"
    expect_eq "${runtime} bare path is whole-file"   "whole=10000" "$(echo "$out" | grep '^whole=')"
    expect_eq "${runtime} missing length rejected"   "nolen=false" "$(echo "$out" | grep '^nolen=')"
    expect_eq "${runtime} negative offset rejected"  "neg=false"   "$(echo "$out" | grep '^neg=')"
    expect_eq "${runtime} zero length rejected"      "zero=false"  "$(echo "$out" | grep '^zero=')"
}

OLDPWD="$(pwd)"
run_rt "lua" "lua"
run_rt "js"  "js"

echo ""
echo "spans-mmap: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
