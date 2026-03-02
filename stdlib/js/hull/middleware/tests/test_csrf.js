// test_csrf.js — Tests for hull:middleware:csrf
//
// Requires crypto and time globals (run via hull test harness).

import { csrf } from "hull:middleware:csrf";

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

// ── generate ─────────────────────────────────────────────────────────

test("generate returns a string with dot separator", () => {
    const token = csrf.generate("sess123", "secret");
    assertEq(typeof token, "string");
    if (token.indexOf(".") < 0)
        throw new Error("expected dot in token");
});

// ── verify ──────────────────────────────────────────────────────────

test("verify accepts valid token", () => {
    const token = csrf.generate("sess456", "mysecret");
    assertEq(csrf.verify(token, "sess456", "mysecret", 3600), true);
});

test("verify rejects wrong session", () => {
    const token = csrf.generate("sess_a", "mysecret");
    assertEq(csrf.verify(token, "sess_b", "mysecret", 3600), false);
});

test("verify rejects wrong secret", () => {
    const token = csrf.generate("sess_c", "secret1");
    assertEq(csrf.verify(token, "sess_c", "secret2", 3600), false);
});

test("verify rejects null token", () => {
    assertEq(csrf.verify(null, "sess", "secret", 3600), false);
});

test("verify rejects malformed token", () => {
    assertEq(csrf.verify("no-dot-here", "sess", "secret", 3600), false);
});

export default { pass, fail };
