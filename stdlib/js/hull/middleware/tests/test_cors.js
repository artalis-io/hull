// test_cors.js — Tests for hull:middleware:cors
//
// Tests pure-function helpers (no runtime globals needed).

import { cors } from "hull:middleware:cors";

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

// ── isAllowedOrigin ──────────────────────────────────────────────────

test("isAllowedOrigin exact match", () => {
    assertEq(cors.isAllowedOrigin("http://a.com", ["http://a.com"]), true);
});

test("isAllowedOrigin wildcard", () => {
    assertEq(cors.isAllowedOrigin("http://any.com", ["*"]), true);
});

test("isAllowedOrigin null origin", () => {
    assertEq(cors.isAllowedOrigin(null, ["*"]), false);
});

test("isAllowedOrigin null origins list", () => {
    assertEq(cors.isAllowedOrigin("http://a.com", null), false);
});

test("isAllowedOrigin miss", () => {
    assertEq(cors.isAllowedOrigin("http://b.com", ["http://a.com"]), false);
});

// ── middleware factory ───────────────────────────────────────────────

test("middleware returns a function", () => {
    const mw = cors.middleware({ origins: ["*"] });
    assertEq(typeof mw, "function");
});

export default { pass, fail };
