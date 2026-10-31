// test_stdlib_audit41.js - stdlib regressions from audit 41 (JS stdlib).
//
// search: a tokenize option must be a primitive string (an object passed the
// regexp test through one toString and was spliced into the SQL through a
// second). totp: recoveryCodes is capped at 32. session: loginHandler's
// audit metadata scrub matches keys without case. See the Lua twin
// (test_stdlib_audit41.lua). The workflow step store cases are in
// test_jobs_hooks.js, the idempotency fingerprint in
// web/middleware/tests/test_idempotency.js.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c).

import { search } from "hull:search";
import { totp } from "hull:web:middleware:totp";
import { session } from "hull:web:middleware:session";
import { cookie } from "hull:web:cookie";

let pass = 0;
let fail = 0;

async function atest(name, fn) {
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
    try { fn(); return false; } catch (e) { return String(e && e.message); }
}

await atest("search: a tokenize option that is not a string is refused", () => {
    let n = 0;
    const evil = { toString() { n++; return n === 1 ? "unicode61" : "x\"); DROP TABLE t; --"; } };
    const err = refused(() => search.createIndex("a41tok", ["body"], { tokenize: evil }));
    assertEq(typeof err === "string" && err.includes("tokenize"), true, String(err));
    assertEq(n, 0, "toString never ran");
    assertEq(typeof refused(() => search.createIndex("a41tok", ["body"], { tokenize: 7 })),
        "string", "a number");
    assertEq(refused(() => search.createIndex("a41tok", ["body"], { tokenize: "porter ascii" })),
        false, "a valid string");
});

await atest("totp: recoveryCodes is capped at 32", () => {
    const T = __testHooks("hull:web:middleware:totp");
    const KEY = "k".repeat(32);
    T.reset();
    const err = refused(() => totp.init({ issuer: "T", encryptionKey: KEY, recoveryCodes: 33 }));
    assertEq(typeof err === "string" && err.includes("recoveryCodes"), true, String(err));
    assertEq(typeof refused(() => totp.init({ issuer: "T", encryptionKey: KEY, recoveryCodes: 1e9 })),
        "string", "1e9 refused");
    T.reset();
    assertEq(refused(() => totp.init({ issuer: "T", encryptionKey: KEY, recoveryCodes: 32 })),
        false, "32 accepted");
    T.reset();
});

await atest("session: the audit metadata scrub matches keys without case", async () => {
    session.init({ cleanup: false });
    let recorded;
    const h = session.loginHandler(cookie, {
        auditLog: { record: (_a, _b, _c, o) => { recorded = o.metadata; } },
        auditMetadata: () => ({ Access_Token: "at", CLAIMS: { sub: "x" }, PassWord: "pw",
                                Secret: "s", factors: "pw" }),
    });
    const hdrs = {};
    const res = {
        status() { return this; }, json() { return this; },
        header(k, v) { hdrs[k] = v; return this; },
    };
    await h({ method: "POST", path: "/login", headers: { host: "app.test" }, ctx: {},
              remote_addr: "192.0.2.7", header: () => null }, res, { id: 41 });
    assertEq(typeof recorded, "object", "recorded");
    assertEq(recorded.factors, "pw", "kept");
    assertEq(recorded.Access_Token, undefined, "Access_Token scrubbed");
    assertEq(recorded.CLAIMS, undefined, "CLAIMS scrubbed");
    assertEq(recorded.PassWord, undefined, "PassWord scrubbed");
    assertEq(recorded.Secret, undefined, "Secret scrubbed");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
