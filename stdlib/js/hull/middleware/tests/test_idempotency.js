// test_idempotency.js — Tests for hull:middleware:idempotency
//
// Requires db, crypto, time, json globals (run via hull test harness).

import { idempotency } from "hull:middleware:idempotency";
import { db } from "hull:db";
import { json } from "hull:json";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        print("FAIL: " + name + ": " + e.message);
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

    const rows = db.query(
        "SELECT state FROM _hull_idempotency_keys WHERE key = ?",
        ["js-key-first-1"]
    );
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

    const rows = db.query(
        "SELECT state, status FROM _hull_idempotency_keys WHERE key = ?",
        ["js-key-complete-1"]
    );
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

// ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", () => {
    const count = idempotency.cleanup();
    assertEq(typeof count, "number");
});

export default { pass, fail };
