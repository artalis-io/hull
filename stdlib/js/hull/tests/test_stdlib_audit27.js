// test_stdlib_audit27.js - stdlib regressions from audit 27 (JS).
//
// csv header order (first-seen), search paging clamps instead of throwing,
// jobs time options as whole seconds, null maxAttempts as unset, olderThan
// 0 kept as 0, count options read the way Lua's tonumber reads them, outbox
// flush's limit, and attachment maxSize: false.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): search / jobs need the db.

import { csv } from "hull:csv";
import { search } from "hull:search";
import { jobs } from "hull:jobs";
import { attachment } from "hull:attachment";
import { time } from "hull:time";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function refused(fn) {
    try { fn(); return false; } catch (_e) { return true; }
}

// ── csv ──────────────────────────────────────────────────────────────

await test("csv.encode: headers keep first-seen order over the union", () => {
    assertEq(csv.encode([{ title: "t", id: 1, done: 0 }], { headers: true }),
             "title,id,done\nt,1,0\n", "first row's order (examples/todo)");
    assertEq(csv.encode([{ b: 1 }, { a: 2, b: 3 }, { c: 4 }], { headers: true }),
             "b,a,c\n1,,\n3,2,\n,,4\n", "later rows append new keys in order");
    assertEq(csv.encode([{ a: 1, b: 2 }], { headers: true, columns: ["b", "a"] }),
             "b,a\n2,1\n", "columns order");
});

// ── search paging ────────────────────────────────────────────────────

await test("search.query: limit / offset clamp instead of throwing", () => {
    search.createIndex("a27docs", ["body"]);
    for (let i = 1; i <= 5; i++) search.index("a27docs", String(i), { body: "alpha " + i });
    const n = (o) => search.query("a27docs", "alpha", o).length;
    assertEq(n({ limit: 5000 }), 5, "limit 5000 clamps to 1000");
    assertEq(n({ limit: 0 }), 1, "limit 0 clamps to 1");
    assertEq(n({ limit: -3 }), 1, "negative clamps to 1");
    assertEq(n({ limit: "2" }), 2, "numeric string");
    assertEq(n({ limit: " 3 " }), 3, "numeric string with spaces");
    assertEq(n({ limit: 2.9 }), 2, "fraction floors");
    assertEq(n({ limit: "abc" }), 5, "garbage takes the default (20)");
    assertEq(n({ limit: NaN }), 5, "NaN takes the default");
    assertEq(n({ limit: true }), 5, "boolean takes the default");
    assertEq(n({ limit: null }), 5, "null takes the default");
    assertEq(n({ limit: Infinity }), 5, "Infinity clamps to 1000");
    assertEq(n({ offset: "3" }), 2, "query-string offset");
    assertEq(n({ offset: -1 }), 5, "negative offset clamps to 0");
    assertEq(n({ offset: "x" }), 5, "garbage offset takes the default (0)");
    assertEq(n({ offset: Infinity }), 0, "Infinity offset clamps to the max");
    assertEq(n({ offset: "Infinity" }), 5, "\"Infinity\" is not a number (Lua's tonumber)");
    assertEq(refused(() => search.query("a27docs", "alpha", { maxQueryLen: "1024" })),
             true, "maxQueryLen stays strict");
});

// ── jobs ─────────────────────────────────────────────────────────────

await test("jobs.enqueue: a fractional throttle works (whole seconds)", () => {
    jobs.init();
    const id = jobs.enqueue("a27thr", {}, { queue: "a27thr", throttle: 0.5 });
    assertEq(typeof id, "number", "first enqueue");
    assertEq(jobs.enqueue("a27thr", {}, { queue: "a27thr", throttle: 2.5 }), null, "throttled");
});

await test("jobs.cleanup: fractional before / olderThan are accepted", async () => {
    jobs.init();
    jobs.handler("a27clean", () => true);
    const id = jobs.enqueue("a27clean", {}, { queue: "a27clean" });
    await jobs.work({ queue: "a27clean", batch: 1 });
    assertEq(jobs.get(id).status, "done", "ran");
    assertEq(jobs.cleanup({ queue: "a27clean", olderThan: 0.5 }), 0, "young row kept");
    assertEq(jobs.cleanup({ queue: "a27clean", olderThan: 0 }), 0, "olderThan 0 accepted");
    assertEq(jobs.cleanup({ queue: "a27clean", before: time.now() - 0.5 }), 0,
             "before rounds up, row kept");
    assertEq(jobs.cleanup({ queue: "a27clean", before: time.now() + 1.5 }), 1,
             "before in the future deletes");
});

await test("jobs: count options read numbers as Lua's tonumber does", async () => {
    jobs.init();
    jobs.handler("a27dead", () => { throw new Error("boom"); });
    jobs.enqueue("a27dead", {}, { queue: "a27dq", maxAttempts: 1 });
    await jobs.work({ queue: "a27dq", batch: 1 });
    const dead = (o) => jobs.dead(Object.assign({ queue: "a27dq" }, o)).length;
    assertEq(dead({}), 1, "one dead");
    assertEq(dead({ offset: "Infinity" }), 1, "\"Infinity\" takes the default offset");
    assertEq(dead({ offset: "0b1" }), 1, "\"0b1\" is not a number in Lua");
    assertEq(dead({ offset: "0x0" }), 1, "hex string");
    assertEq(dead({ offset: Infinity }), 0, "Infinity clamps");
});

await test("jobs: null maxAttempts is unset in init / enqueue / cron", () => {
    jobs.init({ maxAttempts: null });
    assertEq(typeof jobs.enqueue("a27ma", {}, { queue: "a27ma", maxAttempts: null }),
             "number", "enqueue");
    jobs.cron("a27macron", "* * * * *", null, { maxAttempts: null });
    jobs.uncron("a27macron");
});

// ── attachment ───────────────────────────────────────────────────────

await test("attachment.init: maxSize false / null reset the cap", () => {
    attachment.init({ maxSize: 10 });
    attachment.init({ maxSize: false });
    attachment.init({ maxSize: 10 });
    attachment.init({ maxSize: null });
    assertEq(refused(() => attachment.init({ maxSize: true })), true, "true refused");
    assertEq(refused(() => attachment.init({ maxSize: 0 })), true, "0 refused");
});

console.log("stdlib audit 27: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
