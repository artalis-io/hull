// test_stdlib_audit31.js - stdlib regressions from audit 31 (JS stdlib).
//
// The lockout counters decide whether to insert their row with a plain read
// (an UPDATE matching no row took a MySQL gap lock, and two new keys in one
// gap deadlocked); a failed password check bumps the account row before the
// per-IP row, so a failure on the IP row still counts the account; a TOTP
// per-IP failure keeps a lock another instance set; and /login counts an
// unknown address under a key of its own, with the same reads and writes as
// a known one. See the Lua twin (test_stdlib_audit31.lua).
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db.

import { authFlows } from "hull:web:auth-flows";
import { totp } from "hull:web:middleware:totp";
import { crypto } from "hull:crypto";

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

function mkRes() {
    const r = { code: 200, headers: {}, body: undefined };
    r.status = (c) => { r.code = c; return r; };
    r.json = (v) => { r.body = v; return r; };
    r.html = (v) => { r.body = v; return r; };
    r.text = (v) => { r.body = v; return r; };
    r.header = (k, v) => { r.headers[k] = v; return r; };
    r.redirect = (p, c) => { r.code = c || 302; r.location = p; return r; };
    return r;
}

function mkReq(body, ip) {
    return { method: "POST", path: "/auth/login",
             headers: { host: "app.test", "content-type": "application/json",
                        "sec-fetch-site": "same-origin" },
             body: JSON.stringify(body || {}), ctx: {}, remote_addr: ip || "192.0.2.31" };
}

const USERS = {
    "known@a31.test": { id: "a31_known", email: "known@a31.test", email_verified: true,
                        password_hash: crypto.hashPassword("right-password-31") },
};

const AF = __testHooks("hull:web:auth-flows");
AF.reset();
authFlows.init({
    secret: "s".repeat(32),
    trustRequestHost: true,
    emailRateLimit: false,
    maxFailedLogins: 2,
    emailSend: () => {},
    templates: {},
    userFindByEmail: (e) => USERS[e.toLowerCase()] || null,
    userGet: () => null,
    userCreate: () => 1,
    userSetPassword: () => {},
    userSetEmail: () => {},
    userSetEmailVerified: () => {},
    onLogin: (_req, res) => { res.json({ ok: true }); },
});

test("lockout: adjacent new keys of one account are each counted", () => {
    const a = "a31_adj\x1f198.51.100.1";
    const b = "a31_adj\x1f198.51.100.2";
    AF.bumpFailedLogin(a, 5);
    AF.bumpFailedLogin(b, 5);
    AF.bumpFailedLogin(a, 5);
    assertEq(AF.loginFailedCount(a), 2, "a");
    assertEq(AF.loginFailedCount(b), 1, "b");
});

test("lockout: the account row is bumped before the per-IP row", () => {
    const saved = AF.state.maxFailedLogins;
    // The per-IP bump fails (an object is not a SQL parameter); the
    // account's bump, made first, stays counted.
    AF.state.maxFailedLogins = {};
    let ok = true;
    try { AF.bumpFailedLogins("a31_order", "a31_order\x1f203.0.113.9"); }
    catch (_) { ok = false; }
    finally { AF.state.maxFailedLogins = saved; }
    assertEq(ok, false, "the per-IP bump failed");
    assertEq(AF.loginFailedCount("a31_order"), 1, "account counted");
    assertEq(AF.loginFailedCount("a31_order\x1f203.0.113.9"), 0,
             "the per-IP batch rolled back");
});

test("login: an unknown address is counted under its own key", () => {
    const key = AF.unknownLoginKey("Nobody@A31.test");
    assertEq(key, AF.unknownLoginKey("nobody@a31.test"), "ASCII case folded");
    assertEq(key[0], "\x1f", "prefix no account key has");
    assertEq(key.length, "\x1funknown\x1f".length + 64, "sha256 hex");
    const ipKey = key + "\x1f192.0.2.31";
    const res = mkRes();
    AF.handlers.login(mkReq({ email: "nobody@a31.test", password: "wrong-password" }), res);
    assertEq(res.code, 401, "401");
    assertEq(res.body.error, "invalid credentials");
    assertEq(AF.loginFailedCount(key), 1, "account twin");
    assertEq(AF.loginFailedCount(ipKey), 1, "per-IP twin");
    AF.handlers.login(mkReq({ email: "NOBODY@a31.test", password: "wrong-password" }), mkRes());
    assertEq(AF.loginFailedCount(ipKey), 2, "same key, case folded");
    assertEq(AF.lockoutRemaining(ipKey) > 0, true, "locked at maxFailedLogins");
    // Locked: the same answer, and no further count.
    const res3 = mkRes();
    AF.handlers.login(mkReq({ email: "nobody@a31.test", password: "wrong-password" }), res3);
    assertEq(res3.code, 401, "locked 401");
    assertEq(res3.body.error, "invalid credentials");
    assertEq(AF.loginFailedCount(ipKey), 2, "not counted while locked");
});

test("login: both runtimes derive the same unknown-address key", () => {
    // sha256("nobody@a31.test"), as the Lua twin computes it.
    const key = AF.unknownLoginKey("nobody@a31.test");
    assertEq(key, "\x1funknown\x1f6fe6ced6c91c198a591c9a0da895f2ce80321cc9324983f3ecffad3967be7d66",
             "the key");
});

test("login: a known address does the same bookkeeping", () => {
    const ipKey = "a31_known\x1f192.0.2.31";
    const res = mkRes();
    AF.handlers.login(mkReq({ email: "known@a31.test", password: "wrong-password" }), res);
    assertEq(res.code, 401);
    assertEq(res.body.error, "invalid credentials");
    assertEq(AF.loginFailedCount("a31_known"), 1, "account");
    assertEq(AF.loginFailedCount(ipKey), 1, "per-IP");
    const ok = mkRes();
    AF.handlers.login(mkReq({ email: "known@a31.test", password: "right-password-31" }), ok);
    assertEq(ok.code, 200, "right password");
    assertEq(AF.loginFailedCount("a31_known"), 0, "cleared");
});

const T = __testHooks("hull:web:middleware:totp");
T.reset();
totp.init({ issuer: "T", cleanup: false, recoveryCodes: 1,
            maxFailedAttemptsPerIp: 2, lockoutDurationPerIp: 60 });

test("totp: a per-IP failure after the lock keeps it", () => {
    const ip = "203.0.113.31";
    T.bumpFailedAttemptIp(ip, "a31_x");
    T.bumpFailedAttemptIp(ip, "a31_y");
    assertEq(T.lockoutRemainingIp(ip) > 0, true, "locked at the threshold");
    // A failure that passed the check before another instance locked it.
    T.bumpFailedAttemptIp(ip, "a31_x");
    assertEq(T.lockoutRemainingIp(ip) > 0, true, "still locked");
    T.clearFailedAttemptsIp(ip);
    assertEq(T.lockoutRemainingIp(ip), 0, "cleared");
});

test("totp: counters on new and existing rows", () => {
    const ip = "203.0.113.32";
    T.bumpFailedAttemptIp(ip, "a31_z");
    assertEq(T.ipFailedCount(ip), 1, "new row");
    T.creditSuccessIp(ip, "a31_z");
    assertEq(T.ipFailedCount(ip), 0, "credited");
    T.creditSuccessIp("203.0.113.99", "a31_z");   // no row: nothing to credit
    T.bumpFailedAttempt("a31_u");
    T.bumpFailedAttempt("a31_u");
    assertEq(T.lockoutRemaining("a31_u"), 0, "under the per-user max");
    T.clearFailedAttempts("a31_u");
    T.clearFailedAttemptsIp(ip);
});

console.log("stdlib audit 31: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
