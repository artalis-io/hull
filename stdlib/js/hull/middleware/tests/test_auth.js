// test_auth.js — Tests for hull:middleware:auth
//
// Requires db, crypto, time, json globals (run via hull test harness).

import { auth } from "hull:middleware:auth";

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

// ── sessionMiddleware ────────────────────────────────────────────────

test("sessionMiddleware returns a function", () => {
    const mw = auth.sessionMiddleware({ cookieName: "hull.sid" });
    assertEq(typeof mw, "function");
});

// ── jwtMiddleware ───────────────────────────────────────────────────

test("jwtMiddleware returns a function", () => {
    const mw = auth.jwtMiddleware({ secret: "test-secret" });
    assertEq(typeof mw, "function");
});

test("jwtMiddleware requires secret", () => {
    let threw = false;
    try {
        auth.jwtMiddleware({});
    } catch (e) {
        threw = true;
    }
    if (!threw) throw new Error("expected error without secret");
});

export default { pass, fail };
