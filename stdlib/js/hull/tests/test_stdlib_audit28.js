// test_stdlib_audit28.js - stdlib regressions from audit 28 (JS).
//
// Addresses with an unpaired surrogate are refused (auth-flows isEmailIsh,
// email.send); template: safe_url encodes a backslash; jobs duration options
// read a string as Lua's tonumber does ("0b11" / "0o7" refused). The totp
// per-(ip, user) accounting is in test_totp_audit28.js.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): jobs needs the db.

import { authFlows } from "hull:web:auth-flows";
import { email } from "hull:email";
import { template } from "hull:template";
import { jobs } from "hull:jobs";

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

// ── unpaired surrogates in addresses ─────────────────────────────────

const LONE = ["a\uD800@x.co", "a@x\uDC00.co", "a@x.c\uD83D", "\uDFFFa@x.co",
              "a\uDC00\uD800@x.co"];

await test("auth-flows isEmailIsh: an unpaired surrogate is refused", () => {
    const ok = __testHooks("hull:web:auth-flows").isEmailIsh;
    for (const bad of LONE) assertEq(ok(bad), false, "refused " + JSON.stringify(bad));
    assertEq(ok("a😀@x.co"), true, "a paired surrogate (emoji) passes");
    assertEq(ok("a@x.co"), true, "plain");
    assertEq(typeof authFlows, "object");
});

await test("email.send: an address with an unpaired surrogate is refused", async () => {
    for (const bad of LONE) {
        let err = null;
        try {
            await email.send({ from: bad, to: "y@z.co", subject: "s", body: "b" });
        } catch (e) { err = e; }
        assertEq(err !== null, true, "refused " + JSON.stringify(bad));
        assertEq(err.code, "invalid_argument", "code");
        assertEq(err.message, "invalid from address", "message");
    }
});

// ── template: safe_url ─────────────────────────────────────────────

await test("safe_url: a backslash is percent-encoded", () => {
    assertEq(template.renderString("{{{ u | safe_url }}}", { u: "/\\evil.test" }),
             "/%5Cevil.test", "relative");
    assertEq(template.renderString("{{{ u | safe_url }}}", { u: "https://x.test\\@evil.test/" }),
             "https://x.test%5C@evil.test/", "https");
    assertEq(template.renderString("{{ u | safe_url }}", { u: "/a\\b" }),
             "/a%5Cb", "escaped output");
});

// ── jobs: duration options as Lua's tonumber ─────────────────────────

await test("jobs: a duration string with a 0b / 0o prefix is refused", () => {
    for (const bad of ["0b11", "0o7", "0B11", "0O7", "Infinity", "1_000", "abc"]) {
        assertEq(refused(() => jobs.init({ visibilityTimeout: bad })), true,
                 "visibilityTimeout " + JSON.stringify(bad));
        assertEq(refused(() => jobs.reap({ visibilityTimeout: bad })), true,
                 "reap " + JSON.stringify(bad));
    }
    for (const okv of ["30", " 30 ", "0x1e", "1.5", "1e2", 30]) {
        assertEq(refused(() => jobs.init({ visibilityTimeout: okv })), false,
                 "accepted " + JSON.stringify(okv));
    }
    jobs.init();
});

console.log("stdlib audit 28: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
