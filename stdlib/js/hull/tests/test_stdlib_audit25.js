// test_stdlib_audit25.js - stdlib regressions from audit 25 (JS stdlib).
//
// jobs.enqueue takes delay / at / runAt as finite numbers of seconds (or
// numeric strings), whole; a string delay no longer concatenates onto now.
// outbox.enqueue does the same for delay and holds maxAttempts (per call and
// at init) to a whole number >= 0.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): jobs and outbox need the db.

import { jobs } from "hull:jobs";
import { time } from "hull:time";
import { outbox } from "hull:web:middleware:outbox";

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

function typeErr(fn) {
    try { fn(); return false; } catch (e) { return e instanceof TypeError; }
}

// ── jobs ───────────────────────────────────────────────────────────

await test("jobs.enqueue: a numeric-string delay is added, not concatenated", () => {
    jobs.init();
    const before = time.now();
    const id = jobs.enqueue("a25delay", {}, { queue: "a25q", delay: "60" });
    const j = jobs.get(id);
    assertEq(typeof j.runAt, "number", "run_at is a number");
    assertEq(j.runAt >= before + 60 && j.runAt <= time.now() + 60, true,
             "now + 60, got " + j.runAt);
});

await test("jobs.enqueue: delay / at / runAt are whole seconds", () => {
    jobs.init();
    const before = time.now();
    let j = jobs.get(jobs.enqueue("a25frac", {}, { queue: "a25q", delay: 2.5 }));
    assertEq(Number.isInteger(j.runAt), true, "fractional delay rounded");
    assertEq(j.runAt >= before + 3, true, "rounded up");
    j = jobs.get(jobs.enqueue("a25at", {}, { queue: "a25q", at: 1900000000.2 }));
    assertEq(j.runAt, 1900000001, "at rounded up");
    j = jobs.get(jobs.enqueue("a25runat", {}, { queue: "a25q", runAt: "1900000000" }));
    assertEq(j.runAt, 1900000000, "numeric-string runAt");
});

await test("jobs.enqueue: a non-numeric schedule is a TypeError", () => {
    jobs.init();
    for (const k of ["delay", "at", "runAt"]) {
        for (const bad of ["soon", "", "  ", Infinity, -Infinity, NaN, {}, [], true]) {
            const o = { queue: "a25bad" };
            o[k] = bad;
            assertEq(typeErr(() => jobs.enqueue("a25bad", {}, o)), true,
                     k + " = " + String(bad));
        }
    }
});

await test("jobs.enqueue: null / undefined schedule values are absent", () => {
    jobs.init();
    const before = time.now();
    const j = jobs.get(jobs.enqueue("a25null", {}, { queue: "a25q", at: null,
                                                     runAt: undefined, delay: null }));
    assertEq(j.runAt >= before && j.runAt <= time.now(), true, "now");
});

// ── outbox ─────────────────────────────────────────────────────────

await test("outbox.enqueue: a numeric-string delay is added, whole", () => {
    outbox.init();
    const hooks = __testHooks("hull:web:middleware:outbox");
    const before = time.now();
    let id = outbox.enqueue({ kind: "webhook", destination: "http://a25.test/",
                              payload: "{}", delay: "60" });
    let r = hooks.rows(id)[0];
    assertEq(typeof r.next_attempt_at, "number", "a number");
    assertEq(r.next_attempt_at >= before + 60 && r.next_attempt_at <= time.now() + 60,
             true, "now + 60, got " + r.next_attempt_at);
    id = outbox.enqueue({ kind: "webhook", destination: "http://a25.test/",
                          payload: "{}", delay: 1.5 });
    r = hooks.rows(id)[0];
    assertEq(Number.isInteger(r.next_attempt_at), true, "whole seconds");
    hooks.clear();
});

await test("outbox.enqueue: a bad delay or maxAttempts is a TypeError", () => {
    outbox.init();
    const base = { kind: "webhook", destination: "http://a25.test/", payload: "{}" };
    for (const bad of ["soon", Infinity, NaN, {}, true]) {
        assertEq(typeErr(() => outbox.enqueue(Object.assign({ delay: bad }, base))), true,
                 "delay " + String(bad));
    }
    for (const bad of ["5", 2.5, -1, NaN, Infinity, null, 2 ** 31]) {
        assertEq(typeErr(() => outbox.enqueue(Object.assign({ maxAttempts: bad }, base))),
                 true, "maxAttempts " + String(bad));
        assertEq(typeErr(() => outbox.init({ maxAttempts: bad })), true,
                 "init maxAttempts " + String(bad));
    }
    // 0 (no retries) and an ordinary count are fine.
    outbox.enqueue(Object.assign({ maxAttempts: 0 }, base));
    outbox.init({ maxAttempts: 7 });
    outbox.init({ maxAttempts: 5 });
    __testHooks("hull:web:middleware:outbox").clear();
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
console.log("stdlib audit 25: " + pass + " passed, " + fail + " failed");
