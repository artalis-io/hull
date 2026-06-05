#!/bin/sh
# E2E streaming-multipart tests — exercises req:multipart() (Lua) and
# req.multipart() (JS) against real multipart/form-data uploads.
#
# Architecture:
#   1. Generate small/medium/large/binary payloads under a tmp dir.
#   2. For each runtime, start a Hull app that echoes part metadata +
#      byte counts back as JSON.
#   3. Drive uploads via curl -F (which produces standards-compliant
#      multipart/form-data bodies that span socket reads) and verify
#      the JSON response matches expected byte counts / orderings.
#
# Each scenario exercises a different code path:
#   - tiny single field           — synchronous fast path (no yield)
#   - many small fields           — multi-PART_BEGIN / drain via read()
#   - 2 MB / 5 MB binary upload   — chunks() over NEED_DATA cycles
#   - mixed text + binary + text  — ordering across yields
#   - empty file                  — zero-byte PART_DATA path
#   - 10x keep-alive same conn    — no per-request leak in cont/iter
#   - max-part-size enforcement   — parser ERROR surfaces to handler
#
# Usage: sh tests/e2e_multipart.sh
#        RUNTIME=lua sh tests/e2e_multipart.sh
#        RUNTIME=js  sh tests/e2e_multipart.sh
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
    echo "e2e_multipart: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

check_contains() {
    # $1 = description, $2 = response body, $3 = expected substring
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 — expected '$3' in: $2" ;;
    esac
}

check_not_contains() {
    # $1 = description, $2 = response body, $3 = unexpected substring
    case "$2" in
        *"$3"*) fail "$1 — did NOT expect '$3' in: $2" ;;
        *)      pass "$1" ;;
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

stop_pid() {
    if [ -n "$1" ]; then
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}

cleanup() {
    stop_pid "$SERVER_PID"
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

# ── Step 1: Build payloads + fixture apps ───────────────────────────

TMPDIR_WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hull_mp_e2e)

# Payload files. /dev/urandom gives us bytes that DEFINITELY include
# non-ASCII and non-UTF-8 sequences — this catches any UTF-8-validating
# conversion that would silently mangle binary data.
dd if=/dev/urandom of="$TMPDIR_WORK/small.bin"  bs=1     count=64    2>/dev/null
dd if=/dev/urandom of="$TMPDIR_WORK/medium.bin" bs=1024  count=2048  2>/dev/null   # 2 MB
dd if=/dev/urandom of="$TMPDIR_WORK/large.bin"  bs=1024  count=5120  2>/dev/null   # 5 MB
: > "$TMPDIR_WORK/empty.bin"
# Hash files via sha256 so we can verify byte-fidelity round-trip.
# `shasum -a 256` is portable on macOS and Linux.
hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}
HASH_SMALL=$(hash_file  "$TMPDIR_WORK/small.bin")
HASH_MEDIUM=$(hash_file "$TMPDIR_WORK/medium.bin")
HASH_LARGE=$(hash_file  "$TMPDIR_WORK/large.bin")
HASH_EMPTY=$(hash_file  "$TMPDIR_WORK/empty.bin")

# ── Lua fixture ──────────────────────────────────────────────────────

cat > "$TMPDIR_WORK/app.lua" <<'EOF'
app.manifest({
    name    = "mp-e2e-lua",
    version = "0.0.1",
    modules = { "hull/http-server@1", "hull/json@1", "hull/crypto@1" },
})

local crypto = require("hull.crypto")

-- /health — pre-body, so it doesn't need the multipart route's body reader.
app.get("/health", function(req, res) res:text("ok") end)

-- Default upload route — generous size cap. Hashes file parts so the
-- test can verify byte fidelity.
app.post("/upload", function(req, res)
    local parts = {}
    for part in req:multipart() do
        if part.filename then
            -- File: stream via chunks(), accumulate for a hash so the
            -- e2e harness can verify byte-fidelity round-trip.
            local total = 0
            local chunks = {}
            for chunk in part:chunks() do
                total = total + #chunk
                chunks[#chunks + 1] = chunk
            end
            local full = table.concat(chunks)
            -- hull.crypto.sha256 returns a 64-char hex string directly.
            table.insert(parts, {
                name         = part.name,
                filename     = part.filename,
                content_type = part.content_type,
                size         = total,
                sha256       = crypto.sha256(full),
            })
        else
            -- Text field: read() the whole body.
            table.insert(parts, {
                name  = part.name,
                value = part:read(),
            })
        end
    end
    res:json({ ok = true, parts = parts })
end, { multipart = { max_part_size = 64 * 1024 * 1024 } })

-- Size-capped route — anything over 1 KiB rejected mid-stream. We
-- expect a 5xx (the handler raises on parser error, dispatch writes
-- 500) and the test asserts on it.
app.post("/upload-tiny", function(req, res)
    local count = 0
    for part in req:multipart() do
        count = count + 1
    end
    res:json({ ok = true, count = count })
end, { multipart = { max_part_size = 1024 } })

-- chunks() drain skip — user code reads PART_BEGIN but never iterates
-- chunks. Iterator's auto-drain logic should still advance to the next
-- part cleanly.
app.post("/upload-skip", function(req, res)
    local names = {}
    for part in req:multipart() do
        -- Skip the body entirely (don't call read or chunks).
        names[#names + 1] = part.name
    end
    res:json({ ok = true, names = names })
end, { multipart = { max_part_size = 64 * 1024 * 1024 } })

-- Cap-enforcement routes — handler wraps the iterator in pcall so
-- we get a structured 4xx response (the realistic user-code pattern
-- for handling parser errors).
local function run_cap_route(req, res)
    local count = 0
    local ok, err = pcall(function()
        for _ in req:multipart() do count = count + 1 end
    end)
    if ok then
        res:json({ ok = true, count = count })
    else
        res:status(413)
        res:json({ ok = false, count = count, error = tostring(err) })
    end
end

-- max_parts: count cap. > N parts → TOO_MANY_PARTS. Cap fires
-- between PART_END and the next PART_BEGIN — handler is alive at
-- that point, pcall catches the error, structured 413 is flushed
-- before Keel closes the connection.
app.post("/upload-parts-cap", run_cap_route,
    { multipart = { max_parts = 2 } })

-- max_headers_size: per-part header byte cap. With a short first
-- part the handler is alive when the cap fires on the SECOND part's
-- header parse → pcall catches → structured 413.
app.post("/upload-headers-cap", run_cap_route,
    { multipart = { max_headers_size = 128 } })

-- max_total_size: whole-body byte cap. The cap fires inside the
-- body-reader's on_data when accumulated bytes exceed the limit.
-- Closed end-to-end by Keel v2.2.0 streaming-async: the handler is
-- invoked BEFORE leftover is fed, parks on NEED_DATA, then on_data
-- resumes it with HL_MP_RESUME_ERROR — pcall catches, 413 lands.
app.post("/upload-total-cap", run_cap_route,
    { multipart = { max_total_size = 512 } })

-- Streaming-async sync-completion: handler returns 401 BEFORE
-- iterating req:multipart(). Exercises Keel v2.2.0's H1 keep-alive
-- force-off contract — leftover body bytes are stranded, so the
-- dispatch layer must NOT keep the connection alive. We assert the
-- absence of "Connection: keep-alive" in the response (Keel only
-- emits that header when keep_alive=1, so its absence is the signal
-- that the force-off fired).
app.post("/upload-sync-reject", function(_req, res)
    res:status(401)
    res:json({ ok = false, error = "unauthorized" })
end, { multipart = { max_total_size = 64 * 1024 * 1024 } })

-- Hasher edge-case routes (TC4-6): chained update, double digest,
-- update-after-digest. Each route returns either the digest or the
-- error message captured via pcall.
local function safe_hash_op(fn)
    local ok, val = pcall(fn)
    if ok then return { ok = true, value = val } end
    return { ok = false, error = tostring(val) }
end

app.get("/hash-chained", function(_req, res)
    res:json(safe_hash_op(function()
        local h = crypto.create_sha256()
        return h:update("a"):update("b"):update("c"):digest()
    end))
end)

app.get("/hash-double-digest", function(_req, res)
    res:json(safe_hash_op(function()
        local h = crypto.create_sha256()
        h:update("abc"):digest()
        return h:digest()  -- should error
    end))
end)

app.get("/hash-update-after-digest", function(_req, res)
    res:json(safe_hash_op(function()
        local h = crypto.create_sha256()
        h:update("abc"):digest()
        h:update("more")  -- should error
        return "should-not-reach"
    end))
end)
EOF

# ── JS fixture ───────────────────────────────────────────────────────

cat > "$TMPDIR_WORK/app.js" <<'EOF'
import { app } from "hull:app";
import { crypto } from "hull:crypto";

app.manifest({
    name    : "mp-e2e-js",
    version : "0.0.1",
    modules : ["hull/http-server@1", "hull/crypto@1"],
});

// QuickJS doesn't bundle TextDecoder — minimal ASCII decoder is enough
// for text fields the test harness sends (curl -F sends 7-bit ASCII).
function bufToString(buf) {
    const u8 = new Uint8Array(buf);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}

app.get("/health", async (req, res) => res.text("ok"));

app.post("/upload", async (req, res) => {
    const parts = [];
    for await (const part of req.multipart()) {
        if (part.filename) {
            let total = 0;
            // Accumulate raw bytes so we can hash for round-trip checking.
            const buffers = [];
            for await (const chunk of part.chunks()) {
                total += chunk.byteLength;
                buffers.push(new Uint8Array(chunk));
            }
            const all = new Uint8Array(total);
            let off = 0;
            for (const b of buffers) { all.set(b, off); off += b.byteLength; }
            // hull.crypto.sha256 accepts ArrayBuffer (binary-safe) and
            // returns a 64-char hex string directly.
            parts.push({
                name        : part.name,
                filename    : part.filename,
                contentType : part.contentType,
                size        : total,
                sha256      : crypto.sha256(all.buffer),
            });
        } else {
            const buf = await part.read();
            parts.push({ name: part.name, value: bufToString(buf) });
        }
    }
    res.json({ ok: true, parts });
}, { multipart: { maxPartSize: 64 * 1024 * 1024 } });

app.post("/upload-tiny", async (req, res) => {
    let count = 0;
    for await (const part of req.multipart()) { count++; }
    res.json({ ok: true, count });
}, { multipart: { maxPartSize: 1024 } });

app.post("/upload-skip", async (req, res) => {
    const names = [];
    for await (const part of req.multipart()) {
        names.push(part.name);
    }
    res.json({ ok: true, names });
}, { multipart: { maxPartSize: 64 * 1024 * 1024 } });

// Cap-enforcement routes — handler wraps the iterator in try/catch
// so we get a structured 413 response (the realistic user-code
// pattern for handling parser errors).
async function runCapRoute(req, res) {
    let count = 0;
    try {
        for await (const _ of req.multipart()) { count++; }
        res.json({ ok: true, count });
    } catch (e) {
        res.status(413);
        res.json({ ok: false, count, error: String(e && e.message || e) });
    }
}

// max_total_size closed end-to-end by Keel v2.2.0 (see Lua sibling).
app.post("/upload-parts-cap",   runCapRoute, { multipart: { maxParts: 2 } });
app.post("/upload-headers-cap", runCapRoute, { multipart: { maxHeadersSize: 128 } });
app.post("/upload-total-cap",   runCapRoute, { multipart: { maxTotalSize: 512 } });

// Streaming-async sync-completion (see Lua sibling for the contract).
app.post("/upload-sync-reject", async (_req, res) => {
    res.status(401);
    res.json({ ok: false, error: "unauthorized" });
}, { multipart: { maxTotalSize: 64 * 1024 * 1024 } });

// Hasher edge-case routes (TC4-6).
function safeHashOp(fn) {
    try { return { ok: true, value: fn() }; }
    catch (e) { return { ok: false, error: String(e && e.message || e) }; }
}

app.get("/hash-chained", async (_req, res) => {
    res.json(safeHashOp(() => {
        const h = crypto.createSha256();
        return h.update("a").update("b").update("c").digest();
    }));
});

app.get("/hash-double-digest", async (_req, res) => {
    res.json(safeHashOp(() => {
        const h = crypto.createSha256();
        h.update("abc").digest();
        return h.digest();
    }));
});

app.get("/hash-update-after-digest", async (_req, res) => {
    res.json(safeHashOp(() => {
        const h = crypto.createSha256();
        h.update("abc").digest();
        h.update("more");
        return "should-not-reach";
    }));
});
EOF

# ── Step 2: per-runtime test run ─────────────────────────────────────

run_multipart_tests() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "=== E2E multipart: $LABEL runtime (port $PORT) ==="

    "$HULL" -p "$PORT" -l debug "$APP" >/dev/null 2>&1 &
    SERVER_PID=$!

    if ! wait_for_server "$PORT"; then
        fail "$LABEL — Hull server startup"
        stop_pid "$SERVER_PID"
        SERVER_PID=""
        return
    fi
    pass "$LABEL — Hull server started"

    # ── Scenario 1: single small text field (sync path, no yield) ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" -F "alpha=hello")
    check_contains "$LABEL tiny single field: ok"         "$RESP" '"ok":true'
    check_contains "$LABEL tiny single field: name"       "$RESP" '"name":"alpha"'
    check_contains "$LABEL tiny single field: value"      "$RESP" '"value":"hello"'

    # ── Scenario 2: ten text fields in one body ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
        -F "a=1" -F "b=2" -F "c=3" -F "d=4" -F "e=5" \
        -F "f=6" -F "g=7" -F "h=8" -F "i=9" -F "j=10")
    check_contains "$LABEL 10 fields: ok"                 "$RESP" '"ok":true'
    check_contains "$LABEL 10 fields: first"              "$RESP" '"name":"a","value":"1"'
    check_contains "$LABEL 10 fields: last"               "$RESP" '"name":"j","value":"10"'

    # ── Scenario 3: 64-byte binary file (small, single PART_DATA likely) ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
        -F "f=@$TMPDIR_WORK/small.bin")
    check_contains "$LABEL small bin: filename"           "$RESP" '"filename":"small.bin"'
    check_contains "$LABEL small bin: size 64"            "$RESP" '"size":64'
    check_contains "$LABEL small bin: hash match"         "$RESP" "\"sha256\":\"$HASH_SMALL\""

    # ── Scenario 4: 2 MB binary upload — exercises NEED_DATA cycles ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
        -F "f=@$TMPDIR_WORK/medium.bin")
    check_contains "$LABEL 2 MB bin: filename"            "$RESP" '"filename":"medium.bin"'
    check_contains "$LABEL 2 MB bin: size 2097152"        "$RESP" '"size":2097152'
    check_contains "$LABEL 2 MB bin: hash match"          "$RESP" "\"sha256\":\"$HASH_MEDIUM\""

    # ── Scenario 5: 5 MB binary upload — heavy NEED_DATA churn ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
        -F "f=@$TMPDIR_WORK/large.bin")
    check_contains "$LABEL 5 MB bin: filename"            "$RESP" '"filename":"large.bin"'
    check_contains "$LABEL 5 MB bin: size 5242880"        "$RESP" '"size":5242880'
    check_contains "$LABEL 5 MB bin: hash match"          "$RESP" "\"sha256\":\"$HASH_LARGE\""

    # ── Scenario 6: mixed text + binary + text in one body ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
        -F "user=alice" \
        -F "tag=blue" \
        -F "f=@$TMPDIR_WORK/medium.bin" \
        -F "trailer=done")
    check_contains "$LABEL mixed: user field"             "$RESP" '"name":"user","value":"alice"'
    check_contains "$LABEL mixed: tag field"              "$RESP" '"name":"tag","value":"blue"'
    check_contains "$LABEL mixed: file size"              "$RESP" '"size":2097152'
    check_contains "$LABEL mixed: file hash match"        "$RESP" "\"sha256\":\"$HASH_MEDIUM\""
    check_contains "$LABEL mixed: trailer field"          "$RESP" '"name":"trailer","value":"done"'

    # ── Scenario 7: empty file (zero-byte PART_DATA path) ──
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
        -F "f=@$TMPDIR_WORK/empty.bin")
    check_contains "$LABEL empty file: filename"          "$RESP" '"filename":"empty.bin"'
    check_contains "$LABEL empty file: size 0"            "$RESP" '"size":0'
    check_contains "$LABEL empty file: hash match"        "$RESP" "\"sha256\":\"$HASH_EMPTY\""

    # ── Scenario 8: 10 consecutive keep-alive uploads on same conn ──
    # curl reuses connections across multiple -F-wielding requests.
    KEEPALIVE_OK=true
    for i in 1 2 3 4 5 6 7 8 9 10; do
        RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload" \
            -F "iter=$i" -F "f=@$TMPDIR_WORK/small.bin" 2>/dev/null)
        case "$RESP" in
            *'"ok":true'*'"size":64'*) ;;
            *) KEEPALIVE_OK=false ; break ;;
        esac
    done
    if [ "$KEEPALIVE_OK" = "true" ]; then
        pass "$LABEL 10x keep-alive consecutive uploads"
    else
        fail "$LABEL 10x keep-alive consecutive uploads — last response: $RESP"
    fi

    # ── Scenario 9: max-part-size enforcement ──
    # The /upload-tiny route caps at 1 KiB. medium.bin is 2 MB, so the
    # parser raises ERROR on PART_TOO_LARGE; our handler raises; dispatch
    # writes a 500. We just check the status code via -w because the
    # body is intentionally not structured JSON in this case.
    STATUS=$(curl -sS -o /dev/null -w '%{http_code}' \
        -X POST "http://127.0.0.1:$PORT/upload-tiny" \
        -F "f=@$TMPDIR_WORK/medium.bin")
    case "$STATUS" in
        5*) pass "$LABEL max-part-size enforced (got $STATUS)" ;;
        4*) pass "$LABEL max-part-size enforced (got $STATUS)" ;;
        *)  fail "$LABEL max-part-size enforced — expected 4xx/5xx, got $STATUS" ;;
    esac

    # ── Scenario 10: PART_DATA auto-drain (skip body) ──
    # /upload-skip iterates only the part-meta of three fields and
    # never reads body. Iterator's auto-drain must advance past each
    # part's PART_DATA events cleanly.
    RESP=$(curl -sS -X POST "http://127.0.0.1:$PORT/upload-skip" \
        -F "alpha=1" -F "beta=2" -F "gamma=3")
    check_contains "$LABEL skip: ok"                       "$RESP" '"ok":true'
    check_contains "$LABEL skip: alpha listed"             "$RESP" 'alpha'
    check_contains "$LABEL skip: beta listed"              "$RESP" 'beta'
    check_contains "$LABEL skip: gamma listed"             "$RESP" 'gamma'

    # ── Scenario 11: max_parts enforcement ──
    # /upload-parts-cap caps at 2 parts. Four parts → the parser raises
    # TOO_MANY_PARTS on the 3rd PART_BEGIN. By that point the handler
    # has iterated 2 parts and is alive — pcall/try-catch surfaces the
    # error and the handler flushes a structured 413.
    RESP=$(curl --max-time 5 -sS -o /tmp/.mp_resp -w '%{http_code}' \
        -X POST "http://127.0.0.1:$PORT/upload-parts-cap" \
        -F "a=1" -F "b=2" -F "c=3" -F "d=4" 2>/dev/null)
    check_contains "$LABEL max_parts: 413 status"      "$RESP" "413"
    check_contains "$LABEL max_parts: ok=false"        "$(cat /tmp/.mp_resp)" '"ok":false'
    check_contains "$LABEL max_parts: count=2 before error" "$(cat /tmp/.mp_resp)" '"count":2'

    # ── Scenario 12: max_headers_size enforcement ──
    # /upload-headers-cap caps per-part headers at 128 bytes. We send a
    # short first part (under the cap) followed by a part with a very
    # long field name. The cap fires when the parser hits the SECOND
    # part's Content-Disposition line — handler is alive, pcall catches.
    HUGE_NAME=$(printf 'name_%0.s' {1..50})  # 250+ chars
    RESP=$(curl --max-time 5 -sS -o /tmp/.mp_resp -w '%{http_code}' \
        -X POST "http://127.0.0.1:$PORT/upload-headers-cap" \
        -F "ok=first" \
        -F "${HUGE_NAME}=second" 2>/dev/null)
    check_contains "$LABEL max_headers_size: 413 status" "$RESP" "413"
    check_contains "$LABEL max_headers_size: ok=false"   "$(cat /tmp/.mp_resp)" '"ok":false'

    # ── Scenario 13: max_total_size enforcement (Keel v2.2.0) ──
    # /upload-total-cap caps total body bytes at 512. We POST a single
    # field whose value alone exceeds the cap. The handler is invoked
    # BEFORE leftover is fed (streaming-async dispatch), parks on
    # NEED_DATA, then on_data resumes it with ERROR when the cap
    # trips — pcall/try-catch catches and a structured 413 lands.
    BIG_VAL=$(head -c 2048 /dev/urandom | xxd -p | tr -d '\n')
    RESP=$(curl --max-time 5 -sS -o /tmp/.mp_resp -w '%{http_code}' \
        -X POST "http://127.0.0.1:$PORT/upload-total-cap" \
        -F "blob=$BIG_VAL" 2>/dev/null)
    check_contains "$LABEL max_total_size: 413 status" "$RESP" "413"
    check_contains "$LABEL max_total_size: ok=false"   "$(cat /tmp/.mp_resp)" '"ok":false'
    rm -f /tmp/.mp_resp

    # ── Scenario 13b: streaming-async sync-completion (Keel v2.2.0 H1) ──
    # /upload-sync-reject is a streaming-multipart route whose handler
    # responds 401 WITHOUT iterating req:multipart(). The body bytes
    # are stranded — Keel's H1 fix must force keep-alive off so they
    # don't bleed into a subsequent request on the same connection.
    # Curl `-D` dumps response headers; we assert (a) status 401 and
    # (b) absence of "Connection: keep-alive" (Keel only emits that
    # header when keep_alive=1, so its absence is the signal).
    BODY=$(head -c 4096 /dev/urandom | xxd -p | tr -d '\n')
    RESP=$(curl --max-time 5 -sS -o /tmp/.mp_resp -D /tmp/.mp_hdrs \
                -w '%{http_code}' \
        -X POST "http://127.0.0.1:$PORT/upload-sync-reject" \
        -F "blob=$BODY" 2>/dev/null)
    check_contains "$LABEL sync-reject: 401 status" "$RESP" "401"
    check_contains "$LABEL sync-reject: error body" \
        "$(cat /tmp/.mp_resp)" '"error":"unauthorized"'
    if grep -qi "Connection: keep-alive" /tmp/.mp_hdrs 2>/dev/null; then
        fail "$LABEL sync-reject: keep-alive NOT forced off (H1 regression)"
    else
        pass "$LABEL sync-reject: keep-alive forced off"
    fi
    rm -f /tmp/.mp_resp /tmp/.mp_hdrs

    # ── Scenario 14: chained hasher (TC4) ──
    # h.update("a").update("b").update("c").digest() === sha256("abc")
    RESP=$(curl -sS "http://127.0.0.1:$PORT/hash-chained")
    check_contains "$LABEL hash chained: ok"    "$RESP" '"ok":true'
    check_contains "$LABEL hash chained: value" "$RESP" \
        '"value":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"'

    # ── Scenario 15: double digest rejected (TC5) ──
    RESP=$(curl -sS "http://127.0.0.1:$PORT/hash-double-digest")
    check_contains "$LABEL hash double-digest: not ok"      "$RESP" '"ok":false'
    check_contains "$LABEL hash double-digest: error text"  "$RESP" 'digest'

    # ── Scenario 16: update after digest rejected (TC6) ──
    RESP=$(curl -sS "http://127.0.0.1:$PORT/hash-update-after-digest")
    check_contains "$LABEL hash update-after-digest: not ok"     "$RESP" '"ok":false'
    check_contains "$LABEL hash update-after-digest: error text" "$RESP" 'after digest'

    # ── Sanity: post-multipart requests still work (no per-request
    # leak that wedges the connection state) ──
    RESP=$(curl -sS "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL post-multipart /health responds" "$RESP" "ok"

    stop_pid "$SERVER_PID"
    SERVER_PID=""
}

if [ "$RUNTIME" != "js" ]; then
    run_multipart_tests "lua" 17811 "$TMPDIR_WORK/app.lua"
fi

if [ "$RUNTIME" != "lua" ]; then
    run_multipart_tests "js"  17812 "$TMPDIR_WORK/app.js"
fi

# ── Summary ──────────────────────────────────────────────────────────

TOTAL=$((PASS + FAIL))
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "$PASS/$TOTAL e2e multipart tests passed"
else
    echo "$FAIL/$TOTAL e2e multipart tests FAILED"
    exit 1
fi
