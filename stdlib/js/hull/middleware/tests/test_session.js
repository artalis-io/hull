// test_session.js — Tests for hull:middleware:session
//
// Requires db, crypto, time, json globals (run via hull test harness).

import { session } from "hull:middleware:session";

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

test("init creates sessions table", () => {
    session.init({ ttl: 60 });
});

// ── create + load ────────────────────────────────────────────────────

test("create returns 64-char hex ID", () => {
    const id = session.create({ user_id: 1 });
    assertEq(typeof id, "string");
    assertEq(id.length, 64, "length");
});

test("load returns stored data", () => {
    const id = session.create({ user_id: 42 });
    const data = session.load(id);
    if (!data) throw new Error("expected data");
    assertEq(data.user_id, 42);
});

// ── destroy ──────────────────────────────────────────────────────────

test("destroy removes session", () => {
    const id = session.create({ user_id: 99 });
    session.destroy(id);
    const data = session.load(id);
    assertEq(data, null);
});

// ── cleanup ──────────────────────────────────────────────────────────

test("cleanup returns count", () => {
    const count = session.cleanup();
    assertEq(typeof count, "number");
});

export default { pass, fail };
