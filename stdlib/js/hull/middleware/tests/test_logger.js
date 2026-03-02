// test_logger.js — Tests for hull:middleware:logger
//
// Tests pure-function helpers (no runtime globals needed).

import { logger } from "hull:middleware:logger";

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

// ── generateId ───────────────────────────────────────────────────────

test("generateId returns hex string", () => {
    const id = logger.generateId();
    assertEq(typeof id, "string");
    if (id.length === 0) throw new Error("expected non-empty");
});

test("generateId increments", () => {
    const a = logger.generateId();
    const b = logger.generateId();
    if (a === b) throw new Error("expected different IDs");
});

// ── formatLine ───────────────────────────────────────────────────────

test("formatLine basic pairs", () => {
    const line = logger.formatLine([
        ["method", "GET"],
        ["path", "/api"],
    ]);
    assertEq(line, "method=GET path=/api");
});

test("formatLine quotes values with spaces", () => {
    const line = logger.formatLine([
        ["ua", "Mozilla Firefox"],
    ]);
    assertEq(line, 'ua="Mozilla Firefox"');
});

// ── shouldSkip ───────────────────────────────────────────────────────

test("shouldSkip matches exact path", () => {
    assertEq(logger.shouldSkip("/health", ["/health"]), true);
});

test("shouldSkip returns false on miss", () => {
    assertEq(logger.shouldSkip("/api", ["/health"]), false);
});

test("shouldSkip returns false for null list", () => {
    assertEq(logger.shouldSkip("/health", null), false);
});

// ── middleware factory ───────────────────────────────────────────────

test("middleware returns a function", () => {
    const mw = logger.middleware({});
    assertEq(typeof mw, "function");
});

export default { pass, fail };
