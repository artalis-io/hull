// test_stdlib_audit26.js - stdlib option validation from audit 26 (JS).
//
// auditLog retainDays, attachment maxSize, search limit / offset / query
// caps, jobs maxAttempts / priority / throttle / concurrency and cleanup's
// before / olderThan, idempotency / inbox ttl bounds, email addresses with
// C1 controls (NEL), and htmx form errors read as own properties. A bad
// option throws; it no longer deletes every row, disables a cap or retries
// for ever.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): most of these modules need the db.

import { auditLog } from "hull:web:middleware:audit-log";
import { attachment } from "hull:attachment";
import { search } from "hull:search";
import { jobs } from "hull:jobs";
import { idempotency } from "hull:web:middleware:idempotency";
import { inbox } from "hull:web:middleware:inbox";
import { email } from "hull:email";
import { form } from "hull:web:htmx:form";

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

const BAD_NUMS = ["30", "", null, false, true, {}, NaN, Infinity, -Infinity, 0, -1];

// ── auditLog.retainDays ──────────────────────────────────────────────

await test("auditLog.init: a bad retainDays throws and deletes nothing", () => {
    const salt = "a26-salt-0123456789";
    auditLog.init({ fingerprintSalt: salt, cleanup: false });
    auditLog.record(2601, "login", { headers: {}, ip: "10.0.0.1" });
    const before = auditLog.list(2601).length;
    assertEq(before >= 1, true, "a row was recorded");
    for (const bad of BAD_NUMS) {
        assertEq(refused(() => auditLog.init({ fingerprintSalt: salt, retainDays: bad,
                                               cleanup: false })), true, String(bad));
    }
    assertEq(refused(() => auditLog.init({ fingerprintSalt: salt, retainDays: 36501,
                                           cleanup: false })), true, "over 100 years");
    // null used to coerce to 0: a cutoff of "now" deleted every row.
    auditLog.cleanup();
    assertEq(auditLog.list(2601).length, before, "the row survives");
    auditLog.init({ fingerprintSalt: salt, retainDays: 30, cleanup: false });
    auditLog.cleanup();
    assertEq(auditLog.list(2601).length, before, "the row survives retainDays 30");
});

// ── attachment.maxSize ───────────────────────────────────────────────

await test("attachment.init: maxSize must be a finite number > 0 (or null)", () => {
    for (const bad of BAD_NUMS) {
        if (bad === null) continue;   // null = unlimited
        assertEq(refused(() => attachment.init({ maxSize: bad })), true, String(bad));
    }
    attachment.init({ maxSize: 1024 });
    attachment.init({ maxSize: null });
});

// ── search limit / offset / caps ─────────────────────────────────────

await test("search.query: limit / offset / caps are bounded integers", () => {
    search.createIndex("a26docs", ["body"]);
    search.index("a26docs", "1", { body: "alpha one" });
    search.index("a26docs", "2", { body: "alpha two" });
    search.index("a26docs", "3", { body: "alpha three" });
    assertEq(search.query("a26docs", "alpha", { limit: 2 }).length, 2, "limit 2");
    assertEq(search.query("a26docs", "alpha", { limit: 2, offset: 2 }).length, 1, "offset 2");
    for (const bad of ["-1", -1, 0, 1.5, NaN, Infinity, 1001, null, true]) {
        assertEq(refused(() => search.query("a26docs", "alpha", { limit: bad })), true,
                 "limit " + String(bad));
    }
    for (const bad of ["1", -1, 1.5, NaN, Infinity, null]) {
        assertEq(refused(() => search.query("a26docs", "alpha", { offset: bad })), true,
                 "offset " + String(bad));
    }
    for (const k of ["maxQueryLen", "maxQueryTerms"]) {
        for (const bad of ["1024", 0, -1, 1.5, NaN, Infinity, null]) {
            assertEq(refused(() => search.query("a26docs", "alpha", { [k]: bad })), true,
                     k + " " + String(bad));
        }
    }
});

// ── jobs ─────────────────────────────────────────────────────────────

await test("jobs: maxAttempts is an integer in 1..10000", async () => {
    jobs.init();
    for (const bad of ["5", 0, -1, 1.5, NaN, Infinity, 10001, true, {}, null]) {
        assertEq(refused(() => jobs.init({ maxAttempts: bad })), true, "init " + String(bad));
        assertEq(refused(() => jobs.enqueue("a26", {}, { queue: "a26q", maxAttempts: bad })),
                 true, "enqueue " + String(bad));
        if (bad !== null)   // null = no override on a cron schedule
            assertEq(refused(() => jobs.cron("a26cron", "* * * * *", null, { maxAttempts: bad })),
                     true, "cron " + String(bad));
    }
    // A valid one is honoured: maxAttempts 1 dead-letters on the first failure.
    jobs.handler("a26fail", () => { throw new Error("boom"); });
    const id = jobs.enqueue("a26fail", {}, { queue: "a26fq", maxAttempts: 1 });
    await jobs.work({ queue: "a26fq", batch: 1 });
    assertEq(jobs.get(id).status, "dead", "dead after one attempt");
});

await test("jobs.enqueue: priority / throttle / concurrency types", () => {
    jobs.init();
    for (const bad of ["high", 1.5, NaN, Infinity, true]) {
        assertEq(refused(() => jobs.enqueue("a26p", {}, { queue: "a26q", priority: bad })),
                 true, "priority " + String(bad));
    }
    for (const bad of ["soon", -1, NaN, Infinity, true, {}]) {
        assertEq(refused(() => jobs.enqueue("a26t", {}, { queue: "a26q", throttle: bad })),
                 true, "throttle " + String(bad));
    }
    for (const bad of ["2", -1, 1.5, NaN, true]) {
        assertEq(refused(() => jobs.enqueue("a26c", {}, { queue: "a26q",
                                                          concurrencyKey: "k", concurrency: bad })),
                 true, "concurrency " + String(bad));
    }
    assertEq(typeof jobs.enqueue("a26p", {}, { queue: "a26q", priority: -5, throttle: 0,
                                               concurrencyKey: "k", concurrency: 2 }),
             "number", "valid opts");
});

await test("jobs.cleanup: a non-number before throws and deletes nothing", async () => {
    jobs.init();
    jobs.handler("a26clean", () => true);
    const id = jobs.enqueue("a26clean", {}, { queue: "a26clean" });
    await jobs.work({ queue: "a26clean", batch: 1 });
    assertEq(jobs.get(id).status, "done", "the job ran");
    for (const bad of ["x", "", true, {}, NaN, Infinity]) {
        assertEq(refused(() => jobs.cleanup({ queue: "a26clean", before: bad })), true,
                 "before " + String(bad));
    }
    for (const bad of ["x", -1, NaN, Infinity]) {
        assertEq(refused(() => jobs.cleanup({ queue: "a26clean", olderThan: bad })), true,
                 "olderThan " + String(bad));
    }
    assertEq(jobs.get(id) !== null, true, "the done job survives");
});

// ── idempotency / inbox ttl ──────────────────────────────────────────

await test("idempotency / inbox: ttl is a whole number of seconds <= 366 days", () => {
    for (const bad of [31622401, 1e12, 1.5, 0, -1, NaN, Infinity, "60", null]) {
        assertEq(refused(() => idempotency.init({ ttl: bad })), true, "idem init " + String(bad));
        assertEq(refused(() => idempotency.middleware({ ttl: bad })), true,
                 "idem middleware " + String(bad));
        assertEq(refused(() => inbox.init({ ttl: bad })), true, "inbox init " + String(bad));
        assertEq(refused(() => inbox.checkAndMark("a26m", "a26", { ttl: bad })), true,
                 "inbox checkAndMark " + String(bad));
    }
    idempotency.init({ ttl: 31622400 });
    inbox.init({ ttl: 31622400 });
    assertEq(inbox.checkAndMark("a26m", "a26", { ttl: 60 }), false, "new");
});

// ── email addresses ──────────────────────────────────────────────────

await test("email.send: an address with a C1 control (NEL) or U+2028 is refused", async () => {
    for (const bad of ["a\u0085@x.co", "a@x\u0085.co", "a@x.c\u0080o",
                       "a @x.co", "a @x.co"]) {
        let err = null;
        try {
            await email.send({ from: bad, to: "y@z.co", subject: "s", body: "b" });
        } catch (e) { err = e; }
        assertEq(err !== null, true, "refused " + JSON.stringify(bad));
        assertEq(err.code, "invalid_argument", "code");
        assertEq(err.message, "invalid from address", "message");
    }
});

// ── htmx form ────────────────────────────────────────────────────────

await test("htmx form: errors are read as own properties", () => {
    for (const k of ["constructor", "toString", "__proto__", "hasOwnProperty"]) {
        assertEq(form.fieldError({}, k), "", "no own error " + k);
        assertEq(form.fieldAttrs({}, k), "", "no own attrs " + k);
    }
    assertEq(form.fieldError({ constructor: "bad" }, "constructor") !== "", true,
             "an own 'constructor' renders");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
console.log("stdlib audit 26: " + pass + " passed, " + fail + " failed");
