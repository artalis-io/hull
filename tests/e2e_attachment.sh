#!/bin/sh
# E2E tests for hull/attachment@1 (Lua + JS) — PR 1 scope: init,
# store, metadata, read.
#
# Architecture (mirrors e2e_multipart.sh):
#   1. For each runtime, start a Hull app that wires attachment.init +
#      a /upload route that calls attachment.store(part) for each file
#      part and returns the fresh attachment id in JSON.
#   2. Drive uploads via curl -F (real multipart/form-data) and
#      validate the JSON response.
#   3. GET /attachments/<id>/metadata and /attachments/<id>/bytes to
#      exercise the read path.
#
# Scenarios:
#   - happy path: upload PNG, metadata returns sniffed mime=image/png,
#     read returns the same bytes.
#   - dedup: two uploads of identical bytes produce distinct attachment
#     ids that share the same blob_id; both retrieve correctly.
#   - mime allowlist mismatch: app initialized with allowlist=[image/png]
#     rejects a PDF upload with a 4xx.
#   - size cap: app initialized with max_size=1024 rejects a 2-KiB
#     upload with a 4xx.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}
SERVER_PID=""

if [ ! -x "$HULL" ]; then
    echo "e2e_attachment: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ — $2}"; }

contains() {
    # $1 = description, $2 = expected substring, $3 = haystack
    case "$3" in
        *"$2"*) pass "$1" ;;
        *)      fail "$1" "expected substring '$2' in: $3" ;;
    esac
}

wait_for_server() {
    _port=$1
    for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -s "http://127.0.0.1:$_port/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.3
    done
    echo "  server did not start on port $_port"
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

TMPDIR_WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hull_att_e2e)
cleanup() {
    stop_server
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

# ── Payloads ────────────────────────────────────────────────────────

# Real PNG header (8 magic bytes + minimal IHDR-ish padding) so the
# mime sniffer recognises image/png.
printf '\211PNG\r\n\032\n\x00\x00\x00\x0DIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00' > "$TMPDIR_WORK/img.png"
IMG_SIZE=$(wc -c < "$TMPDIR_WORK/img.png" | tr -d ' ')
# Source SHA used to verify the read-path round-trip.
hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}
IMG_SHA=$(hash_file "$TMPDIR_WORK/img.png")
# Same exact bytes for dedup verification (different filename).
cp "$TMPDIR_WORK/img.png" "$TMPDIR_WORK/img2.png"
# PDF magic — used by the mime-allowlist rejection scenario.
printf '%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n' > "$TMPDIR_WORK/doc.pdf"
# 5 KiB PNG-prefixed payload — passes the multipart cap (32 KiB on
# the upload route) AND the allowlist sniff, but trips attachment's
# 4 KiB max_size cap, so the rejection comes from attachment.store
# (not from the multipart parser).
{ printf '\211PNG\r\n\032\n'; dd if=/dev/urandom bs=1024 count=5 2>/dev/null; } > "$TMPDIR_WORK/big.png"

# ── Lua fixture ─────────────────────────────────────────────────────

cat > "$TMPDIR_WORK/app.lua" <<'EOF'
app.manifest({
    name = "att-e2e-lua", version = "0.0.1",
    modules = {
        "hull/attachment@1", "hull/blob@1", "hull/crypto@1",
        "hull/db@1", "hull/http-server@1", "hull/json@1",
    },
    fs = { write = { "data/" } },
})

local attachment = require("hull.attachment")
local blob = require("hull.blob")

-- /health — pre-body, doesn't need multipart route plumbing.
app.get("/health", function(_, res) res:text("ok") end)

-- Initialize storage on startup. blob.init needs the fs cap which
-- gets wired by the sandbox after manifest extraction, so wrap in
-- app.main (runs once on the event-loop thread before serve loop).
app.main(function()
    blob.init({ dir = "data/blobs" })
    attachment.init({
        max_size = 4 * 1024,
        mime_allowlist = { "image/png" },
    })
end)

-- Happy path + dedup. Returns array of {id, blob_id, size} per part.
app.post("/upload", function(req, res)
    local stored = {}
    local ok, err = pcall(function()
        for part in req:multipart() do
            if part.filename then
                local id = attachment.store(part)
                local meta = attachment.metadata(id)
                stored[#stored + 1] = {
                    id = id, blob_id = meta.blob_id, size = meta.size,
                    mime = meta.mime, original_name = meta.original_name,
                }
            end
        end
    end)
    if not ok then
        res:status(413):json({ ok = false, error = tostring(err) })
        return
    end
    res:json({ ok = true, stored = stored })
end, { multipart = { max_part_size = 32 * 1024 } })

-- Metadata + read round-trip.
app.get("/attachments/:id/metadata", function(req, res)
    local meta = attachment.metadata(req.params.id)
    if not meta then res:status(404):json({ error = "not found" }); return end
    res:json(meta)
end)

-- Round-trip verification via SHA-256 over the read() bytes. The
-- PR 2 attachment-serve helper will add binary-safe HTTP responses;
-- until then this is the cleanest way to assert read() returns the
-- exact original bytes.
local crypto = require("hull.crypto")
app.get("/attachments/:id/sha", function(req, res)
    local bytes, err = attachment.read(req.params.id)
    if not bytes then res:status(404):json({ error = err }); return end
    res:json({ sha256 = crypto.sha256(bytes), length = #bytes })
end)
EOF

# ── JS fixture ──────────────────────────────────────────────────────

cat > "$TMPDIR_WORK/app.js" <<'EOF'
import { app } from "hull:app";
import { attachment } from "hull:attachment";
import { blob } from "hull:blob";
import { crypto } from "hull:crypto";

app.manifest({
    name: "att-e2e-js", version: "0.0.1",
    modules: [
        "hull/attachment@1", "hull/blob@1", "hull/crypto@1",
        "hull/db@1", "hull/http-server@1", "hull/json@1",
    ],
    fs: { write: ["data/"] },
});

app.get("/health", (_, res) => res.text("ok"));

// Initialize storage on startup (same reason as Lua: fs cap not
// wired until after manifest extraction).
app.main(() => {
    blob.init({ dir: "data/blobs" });
    attachment.init({
        maxSize: 4 * 1024,
        mimeAllowlist: ["image/png"],
    });
});

app.post("/upload", async (req, res) => {
    const stored = [];
    try {
        for await (const part of req.multipart()) {
            if (part.filename) {
                const id = await attachment.store(part);
                const meta = attachment.metadata(id);
                stored.push({
                    id, blob_id: meta.blob_id, size: meta.size,
                    mime: meta.mime, original_name: meta.original_name,
                });
            }
        }
    } catch (e) {
        res.status(413);
        res.json({ ok: false, error: String(e && e.message || e) });
        return;
    }
    res.json({ ok: true, stored });
}, { multipart: { maxPartSize: 32 * 1024 } });

app.get("/attachments/:id/metadata", (req, res) => {
    const meta = attachment.metadata(req.params.id);
    if (!meta) { res.status(404); res.json({ error: "not found" }); return; }
    res.json(meta);
});

// Round-trip verification via SHA-256 over the read() bytes. See
// the Lua sibling for the rationale (no binary res helper until PR 2).
app.get("/attachments/:id/sha", (req, res) => {
    const bytes = attachment.read(req.params.id);
    if (!bytes) { res.status(404); res.json({ error: "not found" }); return; }
    res.json({ sha256: crypto.sha256(bytes), length: bytes.byteLength });
});
EOF

# ── Per-runtime test driver ─────────────────────────────────────────

run_suite() {
    SUITE=$1
    APP=$2
    PORT=$3
    echo
    echo "=== E2E: hull/attachment@1 ($SUITE) ==="

    # Fresh DB + blob dir per suite so dedup count is deterministic.
    rm -rf "$TMPDIR_WORK/data" "$TMPDIR_WORK/db.sqlite"
    cd "$TMPDIR_WORK"
    "$HULL" "$APP" -p "$PORT" > "$TMPDIR_WORK/server-$SUITE.log" 2>&1 &
    SERVER_PID=$!
    cd - >/dev/null
    if ! wait_for_server "$PORT"; then
        fail "$SUITE — server startup"
        cat "$TMPDIR_WORK/server-$SUITE.log" | head -30
        stop_server
        return
    fi

    # ── Happy path: upload a PNG ────────────────────────────────────
    R=$(curl -s -X POST "http://127.0.0.1:$PORT/upload" \
        -F "file=@$TMPDIR_WORK/img.png")
    contains "$SUITE upload ok"      '"ok":true'           "$R"
    contains "$SUITE sniffed PNG"    '"mime":"image/png"'  "$R"
    contains "$SUITE original name"  '"original_name":"img.png"' "$R"

    # Extract the id for the metadata + bytes round-trip.
    ID1=$(printf '%s' "$R" | sed -n 's/.*"id":"\([0-9a-f]\{32\}\)".*/\1/p' | head -1)
    if [ -z "$ID1" ]; then
        fail "$SUITE failed to parse attachment id"
        echo "    response: $R"
        stop_server; return
    fi
    BLOB1=$(printf '%s' "$R" | sed -n 's/.*"blob_id":"\([0-9a-f]\{64\}\)".*/\1/p' | head -1)

    META=$(curl -s "http://127.0.0.1:$PORT/attachments/$ID1/metadata")
    contains "$SUITE metadata mime"   '"mime":"image/png"'        "$META"
    contains "$SUITE metadata size"   "\"size\":$IMG_SIZE"        "$META"
    contains "$SUITE metadata rc=1"   '"refcount":1'              "$META"

    # Read round-trip via SHA — proves attachment.read() returns the
    # exact original bytes without needing a binary response helper.
    SHA=$(curl -s "http://127.0.0.1:$PORT/attachments/$ID1/sha")
    contains "$SUITE read sha matches" "\"sha256\":\"$IMG_SHA\""  "$SHA"
    contains "$SUITE read length"      "\"length\":$IMG_SIZE"     "$SHA"

    # ── Dedup: re-upload identical bytes (different filename) ───────
    R2=$(curl -s -X POST "http://127.0.0.1:$PORT/upload" \
        -F "file=@$TMPDIR_WORK/img2.png")
    ID2=$(printf '%s' "$R2" | sed -n 's/.*"id":"\([0-9a-f]\{32\}\)".*/\1/p' | head -1)
    BLOB2=$(printf '%s' "$R2" | sed -n 's/.*"blob_id":"\([0-9a-f]\{64\}\)".*/\1/p' | head -1)
    if [ -n "$ID2" ] && [ "$ID1" != "$ID2" ]; then
        pass "$SUITE dedup: distinct ids"
    else
        fail "$SUITE dedup: distinct ids" "id1=$ID1 id2=$ID2"
    fi
    if [ -n "$BLOB1" ] && [ "$BLOB1" = "$BLOB2" ]; then
        pass "$SUITE dedup: same blob_id"
    else
        fail "$SUITE dedup: same blob_id" "blob1=$BLOB1 blob2=$BLOB2"
    fi

    # ── MIME-allowlist rejection: PDF on a png-only app ─────────────
    R3=$(curl -s -X POST "http://127.0.0.1:$PORT/upload" \
        -F "file=@$TMPDIR_WORK/doc.pdf")
    contains "$SUITE allowlist rejects PDF"  '"ok":false'             "$R3"
    contains "$SUITE allowlist err mentions MIME" "MIME not allowed"  "$R3"

    # ── Size-cap rejection: > 4 KiB ─────────────────────────────────
    R4=$(curl -s -X POST "http://127.0.0.1:$PORT/upload" \
        -F "file=@$TMPDIR_WORK/big.png")
    contains "$SUITE size cap rejects"       '"ok":false'             "$R4"
    contains "$SUITE size cap err"            "PART_TOO_LARGE"         "$R4"

    stop_server
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_suite lua "$TMPDIR_WORK/app.lua" 19880
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_suite js "$TMPDIR_WORK/app.js" 19881
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
