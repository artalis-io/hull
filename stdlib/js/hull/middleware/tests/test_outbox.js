// test_outbox.js — Tests for hull:middleware:outbox
//
// Requires db, time, json, http globals (run via hull test harness).
// Note: outbound HTTP calls will fail in test mode (no actual server).

import { outbox } from "hull:middleware:outbox";
import { db } from "hull:db";

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

    const rows = db.query("SELECT state, kind, destination FROM _hull_outbox WHERE id = ?", [id]);
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
    const rows = db.query("SELECT idempotency_key FROM _hull_outbox WHERE id = ?", [id]);
    assertEq(rows[0].idempotency_key, "evt-1-wh-1");
});

test("enqueue with delay", () => {
    const id = outbox.enqueue({
        kind: "http",
        destination: "http://127.0.0.1:9999/delayed",
        payload: "{}",
        delay: 300,
    });
    const rows = db.query("SELECT next_attempt_at, created_at FROM _hull_outbox WHERE id = ?", [id]);
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

// ── flush (delivery fails in test mode) ──────────────────────────────

test("flush processes pending items", () => {
    db.exec("DELETE FROM _hull_outbox");

    outbox.enqueue({
        kind: "webhook",
        destination: "http://127.0.0.1:9999/test-flush",
        payload: '{"event":"flush"}',
    });

    const result = outbox.flush();
    assertEq(typeof result.delivered, "number", "has delivered count");
    assertEq(typeof result.failed, "number", "has failed count");
    assertEq(typeof result.retried, "number", "has retried count");
});

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

export default { pass, fail };
