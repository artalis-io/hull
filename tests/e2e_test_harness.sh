#!/bin/sh
# e2e_test_harness.sh — the in-process `hull test` harness exposes the same
# fs sandbox as `hull dev`, and supports streaming multipart bodies.
#
# Regression guard for the nexogen asset-tracker PLATFORM_GAPS 2026-07-13
# ("Blob storage unavailable in hull test" + "no multipart in test.post"):
#
#   - blob.init / fs.* work under `hull test` (rt->fs_cfg is now wired from
#     the manifest by the shared HlAppContext, mirroring the serve path).
#   - req:multipart() / req.multipart() work in the in-process dispatch: the
#     harness pre-feeds the whole synthetic body to the route's multipart
#     wrapper, so a well-formed body drives the iterator to DONE without a
#     live socket. Previously it raised "no active connection".
#
# Both are checked in Lua AND JS.
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
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ — $2}"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ── Lua fixture ──────────────────────────────────────────────────────
mkdir -p "$TMP/lua/tests"
cat > "$TMP/lua/app.lua" <<'LUA'
local crypto = require("hull.crypto")
app.manifest({
    modules = { "hull/http-server@1", "hull/blob@1", "hull/crypto@1" },
    fs = { write = { "data/blobs" } },
})
app.post("/upload", function(req, res)
    local fields, files = {}, {}
    for part in req:multipart() do
        if part.filename then
            local buf = {}
            for chunk in part:chunks() do buf[#buf + 1] = chunk end
            files[part.name] = { filename = part.filename,
                                 sha = crypto.sha256(table.concat(buf)) }
        else
            fields[part.name] = part:read()
        end
    end
    res:json({ fields = fields, files = files })
end, { multipart = { max_part_size = 1024 * 1024, max_parts = 8 } })
LUA
cat > "$TMP/lua/tests/test_harness.lua" <<'LUA'
local blob = require("hull.blob")
local crypto = require("hull.crypto")

test("blob round-trip under hull test (fs_cfg wired)", function()
    blob.init({ dir = "data/blobs" })
    local id = blob.put("hello nexogen")
    test.ok(id ~= nil, "put returns id")
    test.eq(blob.get(id), "hello nexogen")
    test.ok(blob.exists(id), "exists after put")
end)

test("multipart upload in the in-process dispatch", function()
    local B = "X7HULLBOUNDARY"
    local body = table.concat({
        "--" .. B, 'Content-Disposition: form-data; name="title"', "",
        "Nexogen Asset", "--" .. B,
        'Content-Disposition: form-data; name="doc"; filename="a.txt"',
        "Content-Type: text/plain", "", "hello file", "--" .. B .. "--", "",
    }, "\r\n")
    local r = test.post("/upload", {
        body = body,
        headers = { ["content-type"] = "multipart/form-data; boundary=" .. B },
    })
    test.eq(r.status, 200)
    test.eq(r.json.fields.title, "Nexogen Asset")
    test.eq(r.json.files.doc.filename, "a.txt")
    test.eq(r.json.files.doc.sha, crypto.sha256("hello file"))
end)
LUA

# ── JS fixture ───────────────────────────────────────────────────────
mkdir -p "$TMP/js/tests"
cat > "$TMP/js/app.js" <<'JS'
import { app } from "hull:app";
import { crypto } from "hull:crypto";
app.manifest({
    modules: ["hull/http-server@1", "hull/blob@1", "hull/crypto@1"],
    fs: { write: ["data/blobs"] },
});
app.post("/upload", async (req, res) => {
    const fields = {}, files = {};
    for await (const part of req.multipart()) {
        if (part.filename) {
            const chunks = [];
            for await (const c of part.chunks()) chunks.push(new Uint8Array(c));
            let total = 0; for (const c of chunks) total += c.length;
            const all = new Uint8Array(total); let o = 0;
            for (const c of chunks) { all.set(c, o); o += c.length; }
            files[part.name] = { filename: part.filename, sha: crypto.sha256(all.buffer) };
        } else {
            const buf = await part.read();
            const u = new Uint8Array(buf); let s = "";
            for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
            fields[part.name] = s;
        }
    }
    res.json({ fields, files });
}, { multipart: { maxPartSize: 1024 * 1024, maxParts: 8 } });
JS
cat > "$TMP/js/tests/test_harness.js" <<'JS'
import { blob } from "hull:blob";
import { crypto } from "hull:crypto";

test("blob round-trip under hull test (fs_cfg wired)", () => {
    blob.init({ dir: "data/blobs" });
    const { id } = blob.put("hello nexogen");   // JS put → { id, size }
    test.ok(id, "put returns id");
    test.ok(blob.exists(id), "exists after put");
    const bytes = new Uint8Array(blob.get(id));
    test.eq(bytes.length, 13, "13 bytes round-tripped");
});

test("multipart upload in the in-process dispatch", () => {
    const B = "X7HULLBOUNDARY";
    const body = [
        "--" + B, 'Content-Disposition: form-data; name="title"', "",
        "Nexogen Asset", "--" + B,
        'Content-Disposition: form-data; name="doc"; filename="a.txt"',
        "Content-Type: text/plain", "", "hello file", "--" + B + "--", "",
    ].join("\r\n");
    const r = test.post("/upload", {
        body,
        headers: { "content-type": "multipart/form-data; boundary=" + B },
    });
    test.eq(r.status, 200);
    test.eq(r.json.fields.title, "Nexogen Asset");
    test.eq(r.json.files.doc.filename, "a.txt");
    test.eq(r.json.files.doc.sha, crypto.sha256("hello file"));
});
JS

check_runtime() {
    label="$1"; dir="$2"
    out="$("$HULL" test "$dir" 2>&1)" || true
    # Every test must pass; a "no active connection" / "fs config unavailable"
    # regression shows up as a FAIL line.
    case "$out" in
        *"no active connection"*) fail "$label: multipart in test" "raised 'no active connection'"; echo "$out"; return ;;
        *"fs config unavailable"*) fail "$label: blob in test" "raised 'fs config unavailable'"; echo "$out"; return ;;
    esac
    # 2 tests in the fixture; require both green and none failed.
    case "$out" in
        *"2/2 tests passed"*)
            case "$out" in
                *"FAIL"*) fail "$label" "a test FAILed"; echo "$out" ;;
                *)        pass "$label: blob + multipart under hull test" ;;
            esac ;;
        *) fail "$label" "did not see 2/2 tests passed"; echo "$out" ;;
    esac
}

echo "== Lua =="
check_runtime "lua" "$TMP/lua"
echo "== JS =="
check_runtime "js" "$TMP/js"

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"
[ "$FAIL" -eq 0 ]
