// test_inbox.js — Tests for hull:middleware:inbox
//
// Requires db, time globals (run via hull test harness).

import { inbox } from "hull:middleware:inbox";

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

test("init creates inbox table", () => {
    inbox.init({ ttl: 60 });
});

// ── isDuplicate ─────────────────────────────────────────────────────

test("new message is not a duplicate", () => {
    assertEq(inbox.isDuplicate("js-msg-new-1", "test"), false);
});

test("null messageId returns false", () => {
    assertEq(inbox.isDuplicate(null, "test"), false);
});

test("empty messageId returns false", () => {
    assertEq(inbox.isDuplicate("", "test"), false);
});

// ── markProcessed ───────────────────────────────────────────────────

test("markProcessed then isDuplicate returns true", () => {
    inbox.markProcessed("js-msg-mark-1", "test");
    assertEq(inbox.isDuplicate("js-msg-mark-1", "test"), true);
});

test("different source is not duplicate", () => {
    inbox.markProcessed("js-msg-src-1", "source-A");
    assertEq(inbox.isDuplicate("js-msg-src-1", "source-B"), false);
});

// ── checkAndMark ─────────────────────────────────────────────────────

test("checkAndMark returns false for new message", () => {
    const dup = inbox.checkAndMark("js-msg-cam-1", "test");
    assertEq(dup, false, "first call should not be duplicate");
});

test("checkAndMark returns true for second call", () => {
    inbox.checkAndMark("js-msg-cam-2", "test");
    const dup = inbox.checkAndMark("js-msg-cam-2", "test");
    assertEq(dup, true, "second call should be duplicate");
});

// ── default source ───────────────────────────────────────────────────

test("default source works", () => {
    inbox.markProcessed("js-msg-default-1");
    assertEq(inbox.isDuplicate("js-msg-default-1"), false);
    assertEq(inbox.isDuplicate("js-msg-default-1", "default"), true);
});

// ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", () => {
    const count = inbox.cleanup();
    assertEq(typeof count, "number");
});

export default { pass, fail };
