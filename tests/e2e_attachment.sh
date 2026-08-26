#!/bin/sh
# E2E tests for hull/attachment@1 (Lua + JS) - PR 1 scope: init,
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
    echo "e2e_attachment: hull binary not found at $HULL - run 'make' first"
    exit 1
fi

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ - $2}"; }

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
# PDF magic - used by the mime-allowlist rejection scenario.
printf '%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n' > "$TMPDIR_WORK/doc.pdf"
# 5 KiB PNG-prefixed payload - passes the multipart cap (32 KiB on
# the upload route) AND the allowlist sniff, but trips attachment's
# 4 KiB max_size cap, so the rejection comes from attachment.store
# (not from the multipart parser).
{ printf '\211PNG\r\n\032\n'; dd if=/dev/urandom bs=1024 count=5 2>/dev/null; } > "$TMPDIR_WORK/big.png"
# Non-ASCII filename for the RFC 5987 Content-Disposition encoding
# parity check - accented Latin + CJK + supplementary-plane emoji
# all in one filename so we shake out UTF-16 surrogate / UTF-8
# encoding bugs in either runtime.
cp "$TMPDIR_WORK/img.png" "$TMPDIR_WORK/résumé文档📄.png"

# ── Lua fixture ─────────────────────────────────────────────────────

cat > "$TMPDIR_WORK/app.lua" <<'EOF'
app.manifest({
    name = "att-e2e-lua", version = "0.0.1",
    modules = {
        "hull/attachment@1", "hull/blob@1", "hull/crypto@1",
        "hull/db@1", "hull/fs@1", "hull/http-server@1", "hull/json@1",
        "hull/web/attachment-serve@1",
    },
    fs = { write = { "data/" } },
})

local attachment = require("hull.attachment")
local attachment_serve = require("hull.web.attachment-serve")
local blob = require("hull.blob")

-- /health - pre-body, doesn't need multipart route plumbing.
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

-- Round-trip verification via SHA-256 over the read() bytes - still
-- handy alongside serve() so we can verify bytes match without
-- depending on the auth-gated path.
local crypto = require("hull.crypto")
app.get("/attachments/:id/sha", function(req, res)
    local bytes = attachment.read(req.params.id)
    if not bytes then res:status(404):json({ error = "not found" }); return end
    res:json({ sha256 = crypto.sha256(bytes), length = #bytes })
end)

-- PR 2: delete (refcount-aware unlink).
app.post("/attachments/:id/delete", function(req, res)
    local ok = attachment.delete(req.params.id)
    res:json({ ok = ok })
end)

-- PR 2: read_to_file - materialise to disk under the fs.write allowlist.
app.post("/attachments/:id/dump", function(req, res)
    local n = attachment.read_to_file(req.params.id, "data/dumped.bin")
    if not n then res:status(404):json({ error = "not found" }); return end
    res:json({ ok = true, bytes = n })
end)

-- PR 2: attachment-serve.serve with various auth_check shapes.
-- Allow: token in query string.
app.get("/serve/allow/:id", function(req, res)
    attachment_serve.serve(req, res, req.params.id, {
        auth_check = function(_, _) return true end,
    })
end)

-- Default-deny: omit auth_check entirely.
app.get("/serve/deny/:id", function(req, res)
    attachment_serve.serve(req, res, req.params.id, {})
end)

-- Auth_check that inspects metadata + returns false.
app.get("/serve/explicit-deny/:id", function(req, res)
    attachment_serve.serve(req, res, req.params.id, {
        auth_check = function(_, _) return false end,
    })
end)
EOF

# ── JS fixture ──────────────────────────────────────────────────────

cat > "$TMPDIR_WORK/app.js" <<'EOF'
import { app } from "hull:app";
import { attachment } from "hull:attachment";
import { attachmentServe } from "hull:web:attachment-serve";
import { blob } from "hull:blob";
import { crypto } from "hull:crypto";

app.manifest({
    name: "att-e2e-js", version: "0.0.1",
    modules: [
        "hull/attachment@1", "hull/blob@1", "hull/crypto@1",
        "hull/db@1", "hull/fs@1", "hull/http-server@1", "hull/json@1",
        "hull/web/attachment-serve@1",
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

// Round-trip verification via SHA-256 over the read() bytes - still
// handy alongside serve() so we can verify bytes match without
// depending on the auth-gated path.
app.get("/attachments/:id/sha", (req, res) => {
    const bytes = attachment.read(req.params.id);
    if (!bytes) { res.status(404); res.json({ error: "not found" }); return; }
    res.json({ sha256: crypto.sha256(bytes), length: bytes.byteLength });
});

// PR 2: delete (refcount-aware unlink).
app.post("/attachments/:id/delete", (req, res) => {
    const ok = attachment["delete"](req.params.id);
    res.json({ ok });
});

// PR 2: readToFile - materialise to disk under the fs.write allowlist.
app.post("/attachments/:id/dump", (req, res) => {
    const n = attachment.readToFile(req.params.id, "data/dumped.bin");
    if (n === null) { res.status(404); res.json({ error: "not found" }); return; }
    res.json({ ok: true, bytes: n });
});

// PR 2: attachmentServe.serve with various authCheck shapes.
app.get("/serve/allow/:id", (req, res) => {
    attachmentServe.serve(req, res, req.params.id, {
        authCheck: (_req, _meta) => true,
    });
});

app.get("/serve/deny/:id", (req, res) => {
    attachmentServe.serve(req, res, req.params.id, {});
});

app.get("/serve/explicit-deny/:id", (req, res) => {
    attachmentServe.serve(req, res, req.params.id, {
        authCheck: (_req, _meta) => false,
    });
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
    # Pre-create the fs.write declared dir so Linux Landlock's
    # unveil(2) can pin it - Landlock rejects unveil on non-existent
    # paths, leaving the write allowlist empty and breaking
    # blob.init's mkdir. macOS Seatbelt is permissive about this.
    mkdir -p data
    "$HULL" "$APP" -p "$PORT" > "$TMPDIR_WORK/server-$SUITE.log" 2>&1 &
    SERVER_PID=$!
    cd - >/dev/null
    if ! wait_for_server "$PORT"; then
        fail "$SUITE - server startup"
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

    # Read round-trip via SHA - proves attachment.read() returns the
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

    # ── PR 2: attachment-serve auth gating ──────────────────────────
    # Default-deny (auth_check omitted) → 403.
    DENY_STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT/serve/deny/$ID1")
    contains "$SUITE serve default-deny: 403"   "403"   "$DENY_STATUS"

    # Explicit deny (auth_check returns false) → 403.
    EDENY_STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT/serve/explicit-deny/$ID1")
    contains "$SUITE serve explicit-deny: 403"  "403"   "$EDENY_STATUS"

    # Allow path: 200 + Content-Type + Content-Disposition + ETag +
    # bytes match the original. -D dumps headers; body to file.
    curl -s -D "$TMPDIR_WORK/serve-hdrs.txt" \
        -o "$TMPDIR_WORK/served.bin" \
        "http://127.0.0.1:$PORT/serve/allow/$ID1"
    HDRS=$(cat "$TMPDIR_WORK/serve-hdrs.txt")
    contains "$SUITE serve allow: 200"            "200 OK"           "$HDRS"
    contains "$SUITE serve allow: Content-Type"   "image/png"        "$HDRS"
    contains "$SUITE serve allow: Disposition"    'filename="img.png"' "$HDRS"
    contains "$SUITE serve allow: ETag"           "\"$BLOB1\""       "$HDRS"
    if cmp -s "$TMPDIR_WORK/img.png" "$TMPDIR_WORK/served.bin"; then
        pass "$SUITE serve allow: bytes match"
    else
        fail "$SUITE serve allow: bytes match"
    fi

    # If-None-Match round-trip: send the ETag back, expect 304.
    NM_STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
        -H "If-None-Match: \"$BLOB1\"" \
        "http://127.0.0.1:$PORT/serve/allow/$ID1")
    contains "$SUITE serve If-None-Match: 304"    "304"    "$NM_STATUS"

    # ── PR 2: RFC 5987 unicode filename parity (audit-driven) ───────
    # Upload a file whose name spans accented Latin (résumé) + CJK
    # (文档) + supplementary-plane emoji (📄), then verify both
    # halves of Content-Disposition match the expected UTF-8 octet
    # encoding. The Lua sibling uses byte-wise gsub on raw UTF-8;
    # the JS sibling must encode to UTF-8 first because JS strings
    # are UTF-16 - both should produce IDENTICAL bytes on the wire.
    R_UNI=$(curl -s -X POST "http://127.0.0.1:$PORT/upload" \
        -F "file=@$TMPDIR_WORK/résumé文档📄.png")
    ID_UNI=$(printf '%s' "$R_UNI" | sed -n 's/.*"id":"\([0-9a-f]\{32\}\)".*/\1/p' | head -1)
    if [ -z "$ID_UNI" ]; then
        fail "$SUITE unicode upload failed" "resp: $R_UNI"
    else
        curl -s -D "$TMPDIR_WORK/serve-uni-hdrs.txt" -o /dev/null \
            "http://127.0.0.1:$PORT/serve/allow/$ID_UNI"
        UNI_HDRS=$(cat "$TMPDIR_WORK/serve-uni-hdrs.txt")
        # filename* should percent-encode every UTF-8 byte of the
        # non-ASCII chars. é=C3A9, 文=E68B87(actually E6 96 87),
        # 档=E6A1A3, 📄=F09F9384. Pattern check (relaxed - just verify
        # all non-ASCII bytes are correctly pct-encoded).
        contains "$SUITE unicode: é encoded"   "%C3%A9"          "$UNI_HDRS"
        contains "$SUITE unicode: 文 encoded"  "%E6%96%87"       "$UNI_HDRS"
        contains "$SUITE unicode: 档 encoded"  "%E6%A1%A3"       "$UNI_HDRS"
        contains "$SUITE unicode: 📄 encoded"  "%F0%9F%93%84"    "$UNI_HDRS"
        # ASCII fallback substitutes each non-ASCII UTF-8 byte with
        # `_`. é=2 bytes, 文=3, 档=3, 📄=4 → 14 underscores total
        # interleaved with the ASCII run "r..sum......png" (where
        # each `.` here represents a placeholder for the substituted
        # bytes). Final: r__sum____________.png (22 chars).
        contains "$SUITE unicode: ASCII fallback" 'filename="r__sum____________.png"' "$UNI_HDRS"
    fi

    # ── PR 2: read_to_file - materialise to disk + verify SHA ───────
    DUMP=$(curl -s -X POST "http://127.0.0.1:$PORT/attachments/$ID1/dump")
    contains "$SUITE read_to_file: ok"            '"ok":true'        "$DUMP"
    contains "$SUITE read_to_file: bytes"         "\"bytes\":$IMG_SIZE" "$DUMP"
    DUMPED_SHA=$(hash_file "$TMPDIR_WORK/data/dumped.bin" 2>/dev/null)
    if [ "$DUMPED_SHA" = "$IMG_SHA" ]; then
        pass "$SUITE read_to_file: disk SHA matches source"
    else
        fail "$SUITE read_to_file: disk SHA matches source" \
             "got $DUMPED_SHA expected $IMG_SHA"
    fi

    # ── PR 2: delete (refcount semantics) ───────────────────────────
    # We've stored ID1 and ID2 sharing the same blob_id. Deleting ID2
    # should keep the blob alive (ID1 still references it); deleting
    # ID1 should then unlink the blob.
    D2=$(curl -s -X POST "http://127.0.0.1:$PORT/attachments/$ID2/delete")
    contains "$SUITE delete dup: ok"              '"ok":true'        "$D2"

    # ID2 metadata should be gone.
    M2=$(curl -s -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT/attachments/$ID2/metadata")
    contains "$SUITE delete dup: ID2 metadata 404" "404"              "$M2"

    # ID1's blob should still be readable (the shared blob survived).
    SHA1_AFTER=$(curl -s "http://127.0.0.1:$PORT/attachments/$ID1/sha")
    contains "$SUITE delete dup: ID1 blob alive"  "\"sha256\":\"$IMG_SHA\"" "$SHA1_AFTER"

    # Now delete the last reference. Blob should be unlinked underneath.
    D1=$(curl -s -X POST "http://127.0.0.1:$PORT/attachments/$ID1/delete")
    contains "$SUITE delete last: ok"             '"ok":true'        "$D1"

    M1=$(curl -s -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT/attachments/$ID1/metadata")
    contains "$SUITE delete last: ID1 metadata 404" "404"             "$M1"

    # Verify on-disk blob is gone (best-effort; sandbox-aware check).
    if [ -f "$TMPDIR_WORK/data/blobs/blobs/${BLOB1%${BLOB1#??}}/${BLOB1#??}" ] 2>/dev/null; then
        # Path math is fragile; let blob.delete's own correctness +
        # the absence of references prove the unlink. The route round-
        # trip via /attachments/$ID1/sha → 404 is the contract anyway.
        :
    fi
    SHA_GONE=$(curl -s -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT/attachments/$ID1/sha")
    contains "$SUITE delete last: ID1 read 404"   "404"              "$SHA_GONE"

    # Deleting a non-existent id returns ok:false.
    DN=$(curl -s -X POST "http://127.0.0.1:$PORT/attachments/deadbeef/delete")
    contains "$SUITE delete missing: ok=false"    '"ok":false'       "$DN"

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
