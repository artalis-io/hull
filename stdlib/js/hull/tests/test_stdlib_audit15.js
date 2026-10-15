// test_stdlib_audit15.js - stdlib regressions from audit 15 (JS stdlib).
//
// jobs.init upgrades a database from before its additive columns (the harness
// creates old-schema _hull_jobs / _hull_cron tables first, and checks the
// columns afterwards; the idx_hull_jobs_conc index used to be created before
// the concurrency_key column it indexes). A ttl - session, idempotency, inbox,
// totp's lockoutDurationPerIp - is a positive finite NUMBER in both runtimes:
// a string concatenated onto the expiry (now + "86400"). TOTP secrets are
// sealed bound to their account (["totp", userId]); rows sealed before,
// unbound, still open, and a blob bound to another account does not.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db.

import { jobs } from "hull:jobs";
import { session } from "hull:web:middleware:session";
import { idempotency } from "hull:web:middleware:idempotency";
import { inbox } from "hull:web:middleware:inbox";
import { totp } from "hull:web:middleware:totp";
import { sealbox } from "hull:crypto:sealbox";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function raises(fn, needle, msg) {
    let threw = false, text = "";
    try { fn(); } catch (e) { threw = true; text = String(e && e.message); }
    assertEq(threw, true, (msg || "") + ": throws");
    if (needle) {
        assertEq(text.indexOf(needle) >= 0, true,
                 (msg || "") + ": message names " + needle + " (" + text + ")");
    }
}

const BAD = ["86400", 0, -1, NaN, Infinity, true, null];

// ── jobs.init upgrades an old schema ───────────────────────────────

test("jobs.init adds the missing columns of an old schema", () => {
    jobs.init();
    const id = jobs.enqueue("audit15", { n: 1 });
    assertEq(id != null, true, "enqueue on the upgraded table");
    const job = jobs.get(id);
    assertEq(job && job.progress, 0, "the added progress column is read");
    jobs.init();  // a second init is a no-op
});

// ── ttl options are numbers ────────────────────────────────────────

test("session: ttl / absoluteTtl / per-call ttl must be positive numbers", () => {
    for (const bad of BAD) {
        raises(() => session.init({ ttl: bad }), "ttl", "init ttl " + String(bad));
        raises(() => session.init({ absoluteTtl: bad }), "absoluteTtl",
               "init absoluteTtl " + String(bad));
    }
    session.init({ ttl: 60, absoluteTtl: false });
    session.init({ ttl: 60, absoluteTtl: 3600 });
    const sid = session.create({ user_id: "u1" });
    assertEq(typeof sid, "string");
    for (const bad of BAD) {
        raises(() => session.create({}, { ttl: bad }), "ttl", "create " + String(bad));
        raises(() => session.load(sid, { ttl: bad }), "ttl", "load " + String(bad));
        raises(() => session.update(sid, {}, { ttl: bad }), "ttl", "update " + String(bad));
    }
    assertEq(session.load(sid, { ttl: 120 }).user_id, "u1", "a number ttl works");
});

test("idempotency: init and middleware ttl must be positive numbers", () => {
    for (const bad of BAD) {
        raises(() => idempotency.init({ ttl: bad }), "ttl", "init " + String(bad));
        raises(() => idempotency.middleware({ ttl: bad }), "ttl", "mw " + String(bad));
    }
    idempotency.init({ ttl: 60 });
    assertEq(typeof idempotency.middleware({ ttl: 30 }), "function");
});

test("inbox: init and per-call ttl must be positive numbers", () => {
    for (const bad of BAD) {
        raises(() => inbox.init({ ttl: bad }), "ttl", "init " + String(bad));
        raises(() => inbox.markProcessed("m1", "a15", { ttl: bad }), "ttl", "mark " + String(bad));
        raises(() => inbox.checkAndMark("m2", "a15", { ttl: bad }), "ttl", "check " + String(bad));
    }
    inbox.init({ ttl: 60 });
    assertEq(inbox.checkAndMark("m3", "a15", { ttl: 5 }), false, "new");
    assertEq(inbox.checkAndMark("m3", "a15", { ttl: 5 }), true, "duplicate");
});

test("totp: lockoutDurationPerIp must be a positive number", () => {
    for (const bad of BAD) {
        if (bad === null) continue;  // null keeps the default, like undefined
        totp._test.reset();
        raises(() => totp.init({ lockoutDurationPerIp: bad }), "lockoutDurationPerIp",
               String(bad));
    }
    totp._test.reset();
    totp.init({ lockoutDurationPerIp: 60 });
});

// ── TOTP secrets are bound to their account ────────────────────────

const KEY = "k".repeat(32);

test("totp: a new seal is bound to its account; an old unbound one still opens", () => {
    totp._test.reset();
    totp.init({ issuer: "T", encryptionKey: KEY });
    const secret = "S".repeat(20);
    const enc = totp._test.encryptSecret(secret, "u1");
    assertEq(enc[1], 1); assertEq(enc[2], 1);
    let dec = totp._test.decryptSecret(enc[0], 1, "u1");
    assertEq(dec[0], secret, "opens for its account");
    assertEq(dec[1], 1);
    assertEq(dec[2], undefined, "bound");
    assertEq(totp._test.decryptSecret(enc[0], 1, "u2")[0], null,
             "a blob bound to u1 does not open for u2");
    // A row sealed before audit 15, with no context.
    const ring = sealbox.keyring({ keys: { 1: KEY }, current: 1 });
    const legacy = sealbox.seal(ring, secret);
    dec = totp._test.decryptSecret(legacy, 1, "u1");
    assertEq(dec[0], secret, "an unbound row still opens");
    assertEq(dec[1], 1);
    assertEq(dec[2], true, "flagged for a re-seal");
});

test("totp: enroll + confirm store a bound secret", () => {
    totp._test.reset();
    totp.init({ issuer: "T", encryptionKey: KEY });
    const r = totp.enroll("u15");
    const secret = totp._test.base32Decode(r.secretBase32);
    const code = totp._test.totpAtStep(secret, totp._test.currentStep(), 6);
    assertEq(totp.confirm("u15", code), true, "confirm");
    const row = totp._test.getBlob("u15");
    assertEq(row[1], 1);
    const dec = totp._test.decryptSecret(row[0], 1, "u15");
    assertEq(dec[0], secret, "opens for its account");
    assertEq(dec[2], undefined, "sealed bound");
    assertEq(totp._test.decryptSecret(row[0], 1, "u16")[0], null, "not for another");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
console.log("stdlib audit 15: " + pass + " passed, " + fail + " failed");
