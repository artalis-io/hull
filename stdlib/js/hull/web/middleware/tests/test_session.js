// test_session.js - Tests for hull:web:middleware:session
//
// Requires db, crypto, time, json globals (run via hull test harness).

import { session } from "hull:web:middleware:session";

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

// ── audit 39: request text is stored safely ─────────────────────────

function storable(s, maxUnits) {
    return typeof s === "string" && !s.includes("\u0000") && s.length <= maxUnits
        && !/[\ud800-\udbff](?![\udc00-\udfff])|(?:[^\ud800-\udbff]|^)[\udc00-\udfff]/.test(s);
}

test("audit 39: a hostile User-Agent / IP is stored as safe text", () => {
    // 511 ASCII bytes then an emoji: substring(0, 512) split its pair.
    const ua = "A".repeat(511) + "\u{1F600}tail\u0000";
    const req = { headers: { "user-agent": ua }, remote_addr: "10.0.0.\ud800" };
    const id = session.create({ user_id: "a39" }, { req });
    const list = session.listForUser("a39");
    assertEq(list.length, 1, "one session");
    assertEq(storable(list[0].user_agent, 512), true, "storable ua");
    assertEq(list[0].user_agent, "A".repeat(511), "cut on a code point");
    assertEq(storable(list[0].ip, 64), true, "storable ip");
    session.destroy(id);
});

test("audit 39: destroy of an id that is not a session id is a no-op", () => {
    assertEq(session.destroy("a\u0000b"), false);
    assertEq(session.destroy("z".repeat(64)), false);
    assertEq(session.destroyOthers("x\u0000", "nobody"), 0);
});

// ── cleanup ──────────────────────────────────────────────────────────

test("cleanup returns count", () => {
    const count = session.cleanup();
    assertEq(typeof count, "number");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
