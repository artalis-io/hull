// test_idempotency.js - Tests for hull:web:middleware:idempotency
//
// Runs in the C harness's caps-bearing context (js_stdlib.middleware_*).

import { idempotency } from "hull:web:middleware:idempotency";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
const H = __testHooks('hull:web:middleware:idempotency');
import { json } from "hull:json";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + e.message);
    }
}

function assertEq(a, b, msg) {
    if (a !== b)
        throw new Error((msg || "") + " expected " + b + ", got " + a);
}

// ── Helpers ─────────────────────────────────────────────────────────────

function mockRes() {
    const r = {
        _status: 200,
        _body: null,
        _headers: {},
        status(code) { this._status = code; return this; },
        json(data) { this._body = json.encode(data); return this; },
        text(data) { this._body = data; return this; },
        html(data) { this._body = data; return this; },
        bytes(x) {
            // The replay hands res.bytes an ArrayBuffer (audit 40).
            const u8 = x instanceof ArrayBuffer ? new Uint8Array(x) : x;
            this._byteLength = u8.length;
            let b = "";
            for (let i = 0; i < u8.length; i++) b += String.fromCharCode(u8[i]);
            this._bytes = b;
            return this;
        },
        header(name, value) {
            if (value === undefined) return this._headers[name] || null;
            this._headers[name] = value;
            return this;
        },
    };
    return r;
}

function mockReq(method, path, body, headers) {
    const hdrs = headers || {};
    return {
        method: method || "POST",
        path: path || "/api/test",
        body: body || '{"data":"test"}',
        headers: hdrs,
        ctx: {},
        header(name) {
            // Case-insensitive header lookup
            const lower = name.toLowerCase();
            const keys = Object.keys(hdrs);
            for (let i = 0; i < keys.length; i++) {
                if (keys[i].toLowerCase() === lower)
                    return hdrs[keys[i]];
            }
            return null;
        },
    };
}

// ── init ─────────────────────────────────────────────────────────────

test("init creates idempotency table", () => {
    idempotency.init({ ttl: 60 });
});

// ── middleware: no key ───────────────────────────────────────────────

test("no idempotency key proceeds normally", () => {
    const mw = idempotency.middleware();
    const req = mockReq("POST", "/api/test", '{"x":1}');
    const res = mockRes();
    const result = mw(req, res);
    assertEq(result, 0, "should continue");
});

// ── middleware: GET skipped ──────────────────────────────────────────

test("GET requests are not intercepted", () => {
    const mw = idempotency.middleware();
    const req = mockReq("GET", "/api/test", null, { "idempotency-key": "key-get" });
    const res = mockRes();
    const result = mw(req, res);
    assertEq(result, 0);
});

// ── middleware: first request with key ──────────────────────────────

test("first request with key inserts inflight record", () => {
    const mw = idempotency.middleware();
    const req = mockReq("POST", "/api/items", '{"name":"A"}', { "idempotency-key": "js-key-first-1" });
    const res = mockRes();
    const result = mw(req, res);
    assertEq(result, 0, "should continue to handler");
    assertEq(req.ctx._idem_key, "js-key-first-1");
    assertEq(req.ctx._idem_principal, "__anon");

    const rows = H.rows("js-key-first-1");
    assertEq(rows.length, 1);
    assertEq(rows[0].state, "inflight");
});

// ── respond + cache hit ──────────────────────────────────────────────

test("respond caches response and retry returns cached", () => {
    const mw = idempotency.middleware();

    // First request
    const req1 = mockReq("POST", "/api/items", '{"name":"B"}', { "idempotency-key": "js-key-cache-1" });
    const res1 = mockRes();
    mw(req1, res1);
    idempotency.respond(req1, res1, 201, { id: 42 });

    // Retry with same key and body
    const req2 = mockReq("POST", "/api/items", '{"name":"B"}', { "idempotency-key": "js-key-cache-1" });
    const res2 = mockRes();
    const result = mw(req2, res2);
    assertEq(result, 1, "should short-circuit");
    assertEq(res2._status, 201, "cached status");
    assertEq(res2._headers["X-Idempotency-Replay"], "true");
});

// ── different body = 409 ─────────────────────────────────────────────

test("same key with different body returns 409", () => {
    const mw = idempotency.middleware();

    // First request
    const req1 = mockReq("POST", "/api/items", '{"name":"C"}', { "idempotency-key": "js-key-conflict-1" });
    const res1 = mockRes();
    mw(req1, res1);
    idempotency.respond(req1, res1, 201, { id: 99 });

    // Retry with same key but different body
    const req2 = mockReq("POST", "/api/items", '{"name":"D"}', { "idempotency-key": "js-key-conflict-1" });
    const res2 = mockRes();
    const result = mw(req2, res2);
    assertEq(result, 1, "should short-circuit");
    assertEq(res2._status, 409, "conflict");
});

// ── inflight = 409 ───────────────────────────────────────────────────

test("concurrent request with same key returns 409", () => {
    const mw = idempotency.middleware();

    // First request (in-flight, not completed)
    const req1 = mockReq("POST", "/api/items", '{"name":"E"}', { "idempotency-key": "js-key-inflight-1" });
    const res1 = mockRes();
    mw(req1, res1);

    // Second request with same key
    const req2 = mockReq("POST", "/api/items", '{"name":"E"}', { "idempotency-key": "js-key-inflight-1" });
    const res2 = mockRes();
    const result = mw(req2, res2);
    assertEq(result, 1, "should short-circuit");
    assertEq(res2._status, 409);
});

// ── complete without respond ─────────────────────────────────────────

test("complete marks key done without caching response", () => {
    const mw = idempotency.middleware();

    const req = mockReq("POST", "/api/items", '{"name":"F"}', { "idempotency-key": "js-key-complete-1" });
    const res = mockRes();
    mw(req, res);
    idempotency.complete(req);

    const rows = H.rows("js-key-complete-1");
    assertEq(rows.length, 1);
    assertEq(rows[0].state, "complete");
});

// ── principal scoping ────────────────────────────────────────────────

test("different principals can use same key", () => {
    const mw = idempotency.middleware({
        getPrincipal: (req) => req.ctx.userId || "__anon",
    });

    // User A
    const req1 = mockReq("POST", "/api/items", '{"name":"G"}', { "idempotency-key": "js-key-scope-1" });
    req1.ctx.userId = "user-A";
    const res1 = mockRes();
    mw(req1, res1);
    idempotency.respond(req1, res1, 201, { id: 1 });

    // User B with same key
    const req2 = mockReq("POST", "/api/items", '{"name":"G"}', { "idempotency-key": "js-key-scope-1" });
    req2.ctx.userId = "user-B";
    const res2 = mockRes();
    const result = mw(req2, res2);
    assertEq(result, 0, "different principal, should continue");
});

// ── audit 39: stored byte-exact, before sending; a failed store ─────

test("audit 39: a body with a lone surrogate / NUL replays byte-exact", () => {
    const mw = idempotency.middleware();
    const body = "<p>caf\u00e9 \u0000 \ud83d</p>";
    const req1 = mockReq("POST", "/a39", "x", { "idempotency-key": "a39-bytes" });
    const res1 = mockRes();
    assertEq(mw(req1, res1), 0);
    idempotency.respondHtml(req1, res1, 201, body);
    assertEq(res1._body, body, "first response");
    const row = H.rows("a39-bytes")[0];
    assertEq(row.state, "complete");
    assertEq(row.response_headers.slice(0, 3), "v2:", "v2 format");
    const req2 = mockReq("POST", "/a39", "x", { "idempotency-key": "a39-bytes" });
    const res2 = mockRes();
    assertEq(mw(req2, res2), 1);
    assertEq(res2._status, 201);
    // The WTF-8 bytes of the string: e9 as c3 a9, the lone surrogate as
    // ed a0 bd, the NUL kept.
    assertEq(res2._bytes, "<p>caf\xc3\xa9 \x00 \xed\xa0\xbd</p>", "replayed bytes");
    assertEq(res2._headers["content-type"], "text/html; charset=utf-8");
    assertEq(res2._headers["X-Idempotency-Replay"], "true");
});

test("audit 39: a failed store marks the key processed, never re-runnable", () => {
    const mw = idempotency.middleware();
    let runs = 0;
    const handle = (req, res) => { runs++; idempotency.respond(req, res, 201, { n: runs }); };
    const req1 = mockReq("POST", "/a39f", "x", { "idempotency-key": "a39-fail" });
    const res1 = mockRes();
    assertEq(mw(req1, res1), 0);
    H.failStore(true);
    try { handle(req1, res1); } finally { H.failStore(false); }
    assertEq(res1._status, 201, "the response is still sent");
    assertEq(H.rows("a39-fail")[0].state, "unstored");
    const req2 = mockReq("POST", "/a39f", "x", { "idempotency-key": "a39-fail" });
    const res2 = mockRes();
    assertEq(mw(req2, res2), 1, "a retry never reaches the handler");
    assertEq(res2._status, 409);
    assertEq(runs, 1);
});

test("audit 39: a row in the old format still replays", () => {
    const mw = idempotency.middleware();
    const req = mockReq("POST", "/legacy", "x", { "idempotency-key": "a39-legacy" });
    H.putLegacy("a39-legacy", "__anon", H.computeFingerprint(req), 200, '{"old":true}',
                '{"content-type":"application/json"}');
    const res = mockRes();
    assertEq(mw(req, res), 1);
    assertEq(res._status, 200);
    assertEq(res._body, '{"old":true}');
});

test("audit 39: a key that is not printable ASCII is a 400", () => {
    const mw = idempotency.middleware();
    for (const k of ["caf\u00e9", "a\u0000b", "tab\tkey", "\ud800"]) {
        const res = mockRes();
        assertEq(mw(mockReq("POST", "/k", "x", { "idempotency-key": k }), res), 1);
        assertEq(res._status, 400, "key " + JSON.stringify(k));
    }
});

test("audit 39: a principal the column cannot store is hashed", () => {
    assertEq(H.normPrincipal("u\ud800").slice(0, 7), "sha256:");
    assertEq(H.normPrincipal("u\u0000").slice(0, 7), "sha256:");
});

// ── audit 40: native encode, a size cap before the store ───────────

test("audit 40: the replay sends an ArrayBuffer of the stored bytes", () => {
    const mw = idempotency.middleware();
    const body = "x".repeat(100000) + "\u00e9\ud800";
    const req1 = mockReq("POST", "/a40", "x", { "idempotency-key": "a40-big" });
    assertEq(mw(req1, mockRes()), 0);
    idempotency.respondHtml(req1, mockRes(), 200, body);
    let seen = null;
    const res2 = mockRes();
    res2.bytes = function (x) { seen = x; return this; };
    const req2 = mockReq("POST", "/a40", "x", { "idempotency-key": "a40-big" });
    assertEq(mw(req2, res2), 1);
    assertEq(seen instanceof ArrayBuffer, true, "an ArrayBuffer");
    assertEq(seen.byteLength, 100000 + 2 + 3, "WTF-8 length");
    const u8 = new Uint8Array(seen);
    assertEq(u8[100000], 0xc3); assertEq(u8[100001], 0xa9);
    assertEq(u8[100002], 0xed); assertEq(u8[100003], 0xa0); assertEq(u8[100004], 0x80);
});

test("audit 40: headers past the store cap mark the key unstored", () => {
    const mw = idempotency.middleware();
    let runs = 0;
    const req1 = mockReq("POST", "/a40h", "x", { "idempotency-key": "a40-hdr" });
    const res1 = mockRes();
    assertEq(mw(req1, res1), 0);
    runs++;
    idempotency.respond(req1, res1, 201, { ok: true },
                        { "Content-Language": "a".repeat(300 * 1024) });
    assertEq(res1._status, 201, "the response is still sent");
    assertEq(H.rows("a40-hdr")[0].state, "unstored");
    const res2 = mockRes();
    assertEq(mw(mockReq("POST", "/a40h", "x", { "idempotency-key": "a40-hdr" }), res2), 1);
    assertEq(res2._status, 409);
    assertEq(runs, 1);
});

// ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", () => {
    const count = idempotency.cleanup();
    assertEq(typeof count, "number");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
