#!/bin/sh
# tests/e2e_spans_bind.sh - compute.call({spans=...}) parse + validation
# (mapped-spans, item C). Execution of spans is NOT wired yet
# (item D); this asserts the BINDING contract: the spans list is parsed and
# validated consistently in Lua + JS, malformed entries are rejected before the
# call, and a valid/empty/absent list is a plain call. Because the spans parse
# runs BEFORE module load, a nonexistent module distinguishes a parse rejection
# (raised) from a normal not_found (returned).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL_BIN="${HULL_BIN:-build/hull}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
# expect_contains <label> <needle> <haystack>
expect_contains() {
    case "$3" in
        *"$2"*) pass "$1" ;;
        *)      fail "$1 (expected to contain '$2', got '$3')" ;;
    esac
}

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
head -c 8192 /dev/zero > "$TMPDIR/data.bin"

# ── Lua fixture ─────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.lua" << 'EOF'
local fs = require("hull.fs")
local compute = require("hull.compute")
app.manifest({ modules = { "hull/fs@1", "hull/compute@1" }, fs = { read = { "data.bin" } } })
app.main(function(ctx)
    if not compute.available() then ctx.stdout:write("NOCOMPUTE\n"); return 0 end
    local w = fs.mmap("data.bin", { offset = 0, length = 4096 })
    local function try(opts)
        local ok, r1, r2 = pcall(compute.call, "nomod", "x", opts)
        -- ok=false => a raised spans-validation error; ok=true => compute.call
        -- returned (nil, err) for the missing module (proves spans parsed).
        return (ok and ("ok:" .. tostring(r2)) or ("err:" .. tostring(r1)))
    end
    local o = ctx.stdout
    o:write("valid="  .. try({ spans = { { name = "src", buffer = w } } }) .. "\n")
    o:write("empty="  .. try({ spans = {} }) .. "\n")
    o:write("none="   .. try(nil) .. "\n")
    o:write("noname=" .. try({ spans = { { buffer = w } } }) .. "\n")
    o:write("nobuf="  .. try({ spans = { { name = "a" } } }) .. "\n")
    o:write("dup="    .. try({ spans = { { name = "a", buffer = w }, { name = "a", buffer = w } } }) .. "\n")
    o:write("long="   .. try({ spans = { { name = string.rep("x", 64), buffer = w } } }) .. "\n")
    o:write("notarr=" .. try({ spans = "nope" }) .. "\n")
    local c = fs.mmap("data.bin", { offset = 0, length = 256 }); c:close()
    o:write("closed=" .. try({ spans = { { name = "a", buffer = c } } }) .. "\n")
    w:close()
    return 0
end)
EOF

# ── JS fixture ──────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.js" << 'EOF'
import { app } from "hull:app";
import { fs } from "hull:fs";
import { compute } from "hull:compute";
app.manifest({ modules: ["hull/fs@1", "hull/compute@1"], fs: { read: ["data.bin"] } });
app.main((ctx) => {
    if (!compute.available()) { ctx.stdout.write("NOCOMPUTE\n"); return 0; }
    const w = fs.mmap("data.bin", { offset: 0, length: 4096 });
    const try_ = (opts) => {
        try { const r = compute.call("nomod", "x", opts); return "ok:" + r; }
        catch (e) { return "err:" + (e && e.message ? e.message : e); }
    };
    const o = ctx.stdout;
    o.write("valid="  + try_({ spans: [{ name: "src", buffer: w }] }) + "\n");
    o.write("empty="  + try_({ spans: [] }) + "\n");
    o.write("none="   + try_(undefined) + "\n");
    o.write("noname=" + try_({ spans: [{ buffer: w }] }) + "\n");
    o.write("nobuf="  + try_({ spans: [{ name: "a" }] }) + "\n");
    o.write("dup="    + try_({ spans: [{ name: "a", buffer: w }, { name: "a", buffer: w }] }) + "\n");
    o.write("long="   + try_({ spans: [{ name: "x".repeat(64), buffer: w }] }) + "\n");
    o.write("notarr=" + try_({ spans: "nope" }) + "\n");
    const c = fs.mmap("data.bin", { offset: 0, length: 256 }); c.close();
    o.write("closed=" + try_({ spans: [{ name: "a", buffer: c }] }) + "\n");
    w.close();
    return 0;
});
EOF

run_rt() {
    runtime="$1"; ext="$2"
    echo "--- compute.call spans parse (${runtime}) ---"
    out=$(cd "$TMPDIR" && "$OLDPWD/${HULL_BIN}" run "app.${ext}" 2>/dev/null)
    case "$out" in *NOCOMPUTE*) fail "${runtime} compute unavailable in app.main"; return ;; esac
    # valid / empty / absent spans -> parse OK -> reaches module load -> not_found
    # (Lua returns it, JS throws it; either way NOT a spans-validation error).
    expect_contains "${runtime} valid span parses (not_found)"   "not_found"  "$(echo "$out" | grep '^valid=')"
    expect_contains "${runtime} empty list is a plain call"      "not_found"  "$(echo "$out" | grep '^empty=')"
    expect_contains "${runtime} absent spans is a plain call"    "not_found"  "$(echo "$out" | grep '^none=')"
    # malformed entries -> rejected (raised) before the call.
    expect_contains "${runtime} missing name rejected"          "noname=err:"         "$out"
    expect_contains "${runtime} missing buffer rejected"        "nobuf=err:"          "$out"
    expect_contains "${runtime} duplicate name rejected"        "dup=err:"            "$out"
    expect_contains "${runtime} duplicate name message"         "duplicate span name" "$out"
    expect_contains "${runtime} overlong name rejected"         "long=err:"           "$out"
    expect_contains "${runtime} spans not-array rejected"       "notarr=err:"         "$out"
    expect_contains "${runtime} closed buffer rejected"         "closed=err:"         "$out"
}

OLDPWD="$(pwd)"
run_rt "lua" "lua"
run_rt "js"  "js"

echo ""
echo "spans-bind: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
