// test_stdlib_audit30.js - stdlib regressions from audit 30 (JS stdlib).
//
// The login lockout counter and TOTP's attempt counters are incremented by
// the database (read in the stdlib and written back, concurrent attempts on
// Postgres / MySQL all wrote N+1); their semantics are unchanged: the count
// increments, locks at the threshold, and after an expired login lockout
// starts again at 1. rbac.hasAnyRole / hasAnyPermission take a string as one
// name (a string was iterated character by character). See the Lua twin
// (test_stdlib_audit30.lua); the cron re-registration fix is in
// test_jobs_hooks.js, case audit30.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db.

import { authFlows } from "hull:web:auth-flows";
import { totp } from "hull:web:middleware:totp";
import { rbac } from "hull:web:middleware:rbac";

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

const AF = __testHooks("hull:web:auth-flows");
AF.reset();
authFlows.init({
    secret: "s".repeat(32),
    trustRequestHost: true,
    emailRateLimit: false,
    emailSend: () => {},
    templates: {},
    userFindByEmail: () => null,
    userGet: () => null,
    userCreate: () => 1,
    userSetPassword: () => {},
    userSetEmail: () => {},
    userSetEmailVerified: () => {},
    onLogin: (_req, res) => { res.json({ ok: true }); },
});

test("login lockout: counts, locks at max", () => {
    AF.bumpFailedLogin("a30_u1", 3);
    AF.bumpFailedLogin("a30_u1", 3);
    assertEq(AF.loginFailedCount("a30_u1"), 2, "two counted");
    assertEq(AF.lockoutRemaining("a30_u1"), 0, "not locked yet");
    AF.bumpFailedLogin("a30_u1", 3);
    assertEq(AF.loginFailedCount("a30_u1"), 3, "three counted");
    const rem = AF.lockoutRemaining("a30_u1");
    assertEq(rem > 0 && rem <= AF.state.lockoutDuration, true, "locked " + rem);
});

test("login lockout: the count starts again after an expired window", () => {
    const saved = AF.state.lockoutDuration;
    AF.state.lockoutDuration = -5;   // the lock it sets is already over
    try {
        for (let i = 0; i < 3; i++) AF.bumpFailedLogin("a30_u2", 3);
        assertEq(AF.loginFailedCount("a30_u2"), 3, "locked count");
        assertEq(AF.lockoutRemaining("a30_u2"), 0, "window over");
        AF.state.lockoutDuration = saved;
        AF.bumpFailedLogin("a30_u2", 3);
        assertEq(AF.loginFailedCount("a30_u2"), 1, "restarted");
        assertEq(AF.lockoutRemaining("a30_u2"), 0, "the old lock cleared");
        AF.bumpFailedLogin("a30_u2", 3);
        AF.bumpFailedLogin("a30_u2", 3);
        assertEq(AF.lockoutRemaining("a30_u2") > 0, true, "locks again at max");
    } finally {
        AF.state.lockoutDuration = saved;
    }
});

test("login lockout: max 1 locks on the first failure", () => {
    AF.bumpFailedLogin("a30_u3", 1);
    assertEq(AF.loginFailedCount("a30_u3"), 1);
    assertEq(AF.lockoutRemaining("a30_u3") > 0, true, "locked");
});

const T = __testHooks("hull:web:middleware:totp");
const lockouts = [];
T.reset();
totp.init({ issuer: "T", cleanup: false, recoveryCodes: 1,
            maxFailedAttempts: 2, lockoutDuration: 60, maxLockoutDuration: 600,
            onLockout: (uid, until, n) => { lockouts.push([uid, until, n]); } });

test("totp: the per-user count locks every max-th failure, escalating", () => {
    T.bumpFailedAttempt("a30_t1");
    assertEq(T.lockoutRemaining("a30_t1"), 0, "one failure");
    assertEq(lockouts.length, 0);
    T.bumpFailedAttempt("a30_t1");
    const r1 = T.lockoutRemaining("a30_t1");
    assertEq(r1 > 0 && r1 <= 60, true, "first lockout " + r1);
    assertEq(lockouts.length, 1, "onLockout");
    assertEq(lockouts[0][2], 1, "n = 1");
    T.bumpFailedAttempt("a30_t1");
    assertEq(lockouts.length, 1, "not at the threshold");
    T.bumpFailedAttempt("a30_t1");
    assertEq(lockouts.length, 2, "second lockout");
    assertEq(lockouts[1][2], 2, "n = 2");
    const r2 = T.lockoutRemaining("a30_t1");
    assertEq(r2 > 60 && r2 <= 120, true, "doubled " + r2);
    T.clearFailedAttempts("a30_t1");
    assertEq(T.lockoutRemaining("a30_t1"), 0, "cleared");
});

test("totp: the per-IP count and its per-user shares", () => {
    const ip = "203.0.113.30";
    T.bumpFailedAttemptIp(ip, "a30_x");
    T.bumpFailedAttemptIp(ip, "a30_x");
    T.bumpFailedAttemptIp(ip, "a30_y");
    assertEq(T.ipFailedCount(ip), 3, "three counted");
    T.creditSuccessIp(ip, "a30_x");
    assertEq(T.ipFailedCount(ip), 1, "x's two came off");
    T.creditSuccessIp(ip, "a30_x");
    assertEq(T.ipFailedCount(ip), 1, "credited once");
    T.clearFailedAttemptsIp(ip);
});

rbac.init();
rbac.assign("a30_r1", "admin");
rbac.assign("a30_r2", "d");
rbac.defineRole("editor", ["posts.write"]);
rbac.assign("a30_r3", "editor");

test("rbac: hasAnyRole takes a string as one role", () => {
    assertEq(rbac.hasAnyRole("a30_r1", "admin"), true, "admin");
    assertEq(rbac.hasAnyRole("a30_r2", "admin"), false, "a role named 'd' is not admin");
    assertEq(rbac.hasAnyRole("a30_r1", ["x", "admin"]), true, "list");
});

test("rbac: hasAnyPermission takes a string as one permission", () => {
    assertEq(rbac.hasAnyPermission("a30_r3", "posts.write"), true, "string");
    assertEq(rbac.hasAnyPermission("a30_r3", "p"), false, "no such permission");
    assertEq(rbac.hasAnyPermission("a30_r3", ["posts.write"]), true, "list");
});

console.log("stdlib audit 30: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
