// test_outbox.js - Tests for hull:web:middleware:outbox
//
// Requires db, time, json, http globals (run via hull test harness).
// Note: outbound HTTP calls will fail in test mode (no actual server).

import { outbox } from "hull:web:middleware:outbox";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
const H = __testHooks('hull:web:middleware:outbox');
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

// ── init ─────────────────────────────────────────────────────────────

test("init creates outbox table", () => {
    outbox.init({ maxAttempts: 3 });
});

// ── enqueue ─────────────────────────────────────────────────────────

test("enqueue inserts a pending item", () => {
    const id = outbox.enqueue({
        kind: "webhook",
        destination: "http://127.0.0.1:9999/hook",
        payload: '{"event":"test"}',
    });
    if (id <= 0) throw new Error("expected positive ID");

    const rows = H.rows(id);
    assertEq(rows.length, 1);
    assertEq(rows[0].state, "pending");
    assertEq(rows[0].kind, "webhook");
});

test("enqueue with idempotency key", () => {
    const id = outbox.enqueue({
        kind: "webhook",
        destination: "http://127.0.0.1:9999/hook",
        payload: '{"event":"test2"}',
        idempotencyKey: "evt-1-wh-1",
    });
    const rows = H.rows(id);
    assertEq(rows[0].idempotency_key, "evt-1-wh-1");
});

test("enqueue with delay", () => {
    const id = outbox.enqueue({
        kind: "http",
        destination: "http://127.0.0.1:9999/delayed",
        payload: "{}",
        delay: 300,
    });
    const rows = H.rows(id);
    if (rows[0].next_attempt_at <= rows[0].created_at)
        throw new Error("delay should push next_attempt_at");
});

test("enqueue requires kind", () => {
    let caught = false;
    try {
        outbox.enqueue({ destination: "http://x", payload: "{}" });
    } catch (e) {
        caught = true;
    }
    assertEq(caught, true);
});

test("enqueue requires destination", () => {
    let caught = false;
    try {
        outbox.enqueue({ kind: "webhook", payload: "{}" });
    } catch (e) {
        caught = true;
    }
    assertEq(caught, true);
});

test("enqueue requires payload", () => {
    let caught = false;
    try {
        outbox.enqueue({ kind: "webhook", destination: "http://x" });
    } catch (e) {
        caught = true;
    }
    assertEq(caught, true);
});

// ── flush (delivery stubbed to fail) ─────────────────────────────────

// flush is async and the harness drives no network, so delivery goes
// through outbox's own seam: a 503 the flush retries.
try {
    H.clear();
    outbox.enqueue({
        kind: "webhook",
        destination: "http://127.0.0.1:9999/test-flush",
        payload: '{"event":"flush"}',
    });
    H.setPost(async () => ({ status: 503 }));
    let result;
    try { result = await outbox.flush(); } finally { H.setPost(null); }
    assertEq(typeof result.delivered, "number", "has delivered count");
    assertEq(typeof result.failed, "number", "has failed count");
    assertEq(typeof result.retried, "number", "has retried count");
    assertEq(result.delivered + result.failed + result.retried, 1, "one item handled");
    pass++;
} catch (e) {
    fail++;
    console.log("FAIL: flush processes pending items: " + e.message);
}

// Audit 39: the delivery error is remote / runtime text, stored as safe text
// (a lone surrogate or a NUL failed the outcome UPDATE on Postgres).
try {
    H.clear();
    const id = outbox.enqueue({
        kind: "webhook",
        destination: "http://127.0.0.1:9999/a39",
        payload: '{"event":"a39"}',
    });
    H.setPost(async () => {
        throw new Error("upstream: a\u0000b \ud83d " + "y".repeat(70000));
    });
    try { await outbox.flush(); } finally { H.setPost(null); }
    const row = H.rows(id)[0];
    assertEq(row.attempts, 1, "attempt counted");
    const le = row.last_error;
    assertEq(typeof le, "string", "last_error");
    assertEq(le.includes("\u0000"), false, "no NUL");
    assertEq(le.includes("\ufffd"), true, "U+FFFD");
    assertEq(le.length <= 4099, true, "capped");
    pass++;
} catch (e) {
    fail++;
    console.log("FAIL: audit 39 hostile delivery error: " + e.message);
}

test("audit 39: an idempotencyKey the column cannot store is refused", () => {
    for (const k of ["a\u0000b", "\ud800", "k".repeat(256)]) {
        let caught = null;
        try {
            outbox.enqueue({ kind: "webhook", destination: "http://127.0.0.1:9999/k",
                             payload: "{}", idempotencyKey: k });
        } catch (e) { caught = e; }
        assertEq(caught !== null && String(caught.message).includes("idempotencyKey"), true,
                 String(caught));
    }
});

// Audit 23: an app header "idempotency-key" was sent beside the row's
// "Idempotency-Key" - two headers.
try {
    H.clear();
    outbox.enqueue({
        kind: "webhook",
        destination: "http://127.0.0.1:9999/test-flush",
        payload: '{"event":"case"}',
        headers: JSON.stringify({ "idempotency-key": "app-key" }),
        idempotencyKey: "row-key",
    });
    let seen = null;
    H.setPost(async (_url, _body, opts) => { seen = opts.headers; return { status: 200 }; });
    try { await outbox.flush(); } finally { H.setPost(null); }
    assertEq(seen !== null, true, "posted");
    const keys = Object.keys(seen).filter((k) => k.toLowerCase() === "idempotency-key");
    assertEq(keys.length, 1, "one Idempotency-Key");
    assertEq(seen["idempotency-key"], "app-key", "the app's own");
    pass++;
} catch (e) {
    fail++;
    console.log("FAIL: flush adds no second Idempotency-Key in another case: " + e.message);
}

// ── stats ───────────────────────────────────────────────────────────

test("stats returns counts by state", () => {
    const s = outbox.stats();
    assertEq(typeof s.pending, "number", "has pending");
    assertEq(typeof s.delivered, "number", "has delivered");
    assertEq(typeof s.failed, "number", "has failed");
});

// ── middleware ───────────────────────────────────────────────────────

test("middleware sets _outbox_flush flag", () => {
    const mw = outbox.middleware();
    const req = { ctx: {}, header() { return null; } };
    const res = {};
    const result = mw(req, res);
    assertEq(result, 0);
    assertEq(req.ctx._outbox_flush, true);
});

// ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", () => {
    const count = outbox.cleanup(0);
    assertEq(typeof count, "number");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
