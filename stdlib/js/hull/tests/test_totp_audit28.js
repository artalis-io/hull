// test_totp_audit28.js - TOTP regressions from audit 28 (JS stdlib).
//
// A successful verify takes only that user's own failures from an IP back
// out of the IP's count; failures spread over other accounts still count.
// See the Lua twin (test_totp_audit28.lua).
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db. Each
// enrolment hashes its recovery codes, so the suite enrolls few accounts and
// records most failures through the bump hook.

import { totp } from "hull:web:middleware:totp";

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

const H = () => __testHooks("hull:web:middleware:totp");

function enroll(uid) {
    const r = totp.enroll(uid);
    const secret = H().base32Decode(r.secretBase32);
    const code = H().totpAtStep(secret, H().currentStep(), 6);
    assertEq(totp.confirm(uid, code), true, "confirm " + uid);
    return [secret, r.recoveryCodes];
}

// A code at step current + k (confirm used the current one).
const codeAt = (secret, k) => H().totpAtStep(secret, H().currentStep() + k, 6);

function init(perIp) {
    H().reset();
    totp.init({ issuer: "T", cleanup: false, recoveryCodes: 1,
                maxFailedAttemptsPerIp: perIp });
}

test("totp: a user's success takes its own failures off the IP count", () => {
    init(5);
    const [sa, rc] = enroll("a28_a1");
    const ip = "198.51.100.1";
    const req = { remote_addr: ip, headers: {} };
    assertEq(totp.verify("a28_a1", "000000x", req), false);
    assertEq(totp.verify("a28_a1", "000000y", req), false);
    assertEq(H().ipFailedCount(ip), 2, "two failures counted");
    assertEq(totp.verify("a28_a1", codeAt(sa, 1), req), true, "own code verifies");
    assertEq(H().ipFailedCount(ip), 0, "the user's own failures came off");
    // The recovery-code path credits the same way.
    H().bumpFailedAttemptIp(ip, "a28_other");
    assertEq(totp.verify("a28_a1", "000000z", req), false);
    assertEq(H().ipFailedCount(ip), 2);
    assertEq(totp.verify("a28_a1", rc[0], req), true, "recovery code verifies");
    assertEq(H().ipFailedCount(ip), 1, "only a1's failure came off");
});

test("totp: typos from many users, each then verifying, never lock the IP", () => {
    init(3);
    const ip = "198.51.100.5";
    for (let i = 1; i <= 10; i++) {
        H().bumpFailedAttemptIp(ip, "a28_t" + i);
        H().creditSuccessIp(ip, "a28_t" + i);
    }
    assertEq(H().ipFailedCount(ip), 0);
    assertEq(H().lockoutRemainingIp(ip), 0, "no lockout from honest typos");
});

test("totp: another account's failures stay after a success", () => {
    init(5);
    const [sa] = enroll("a28_a2");
    const ip = "198.51.100.2";
    const req = { remote_addr: ip, headers: {} };
    for (let i = 0; i < 3; i++) H().bumpFailedAttemptIp(ip, "a28_b2");
    assertEq(totp.verify("a28_a2", "000000z", req), false);
    assertEq(H().ipFailedCount(ip), 4);
    assertEq(totp.verify("a28_a2", codeAt(sa, 1), req), true, "attacker's own code");
    assertEq(H().ipFailedCount(ip), 3, "only a2's one failure came off");
    assertEq(totp.verify("a28_a2", "000000v", req), false);
    assertEq(totp.verify("a28_a2", "000000u", req), false);
    assertEq(H().lockoutRemainingIp(ip) > 0, true,
             "b2's failures still count toward the lockout");
});

test("totp: failures from distinct users with no success still lock the IP", () => {
    init(20);
    const ip = "198.51.100.3";
    for (let i = 1; i <= 19; i++) {
        H().bumpFailedAttemptIp(ip, "a28_d" + i);
        assertEq(H().lockoutRemainingIp(ip), 0, "not locked after " + i);
    }
    assertEq(H().ipFailedCount(ip), 19);
    H().bumpFailedAttemptIp(ip, "a28_d20");
    assertEq(H().lockoutRemainingIp(ip) > 0, true, "20 failures lock the IP");
});

test("totp: a lockout drops the shares, so a later success takes nothing", () => {
    init(3);
    const ip = "198.51.100.4";
    for (let i = 0; i < 3; i++) H().bumpFailedAttemptIp(ip, "a28_e1");
    assertEq(H().lockoutRemainingIp(ip) > 0, true, "locked");
    assertEq(H().ipFailedCount(ip), 0, "the count starts again");
    H().creditSuccessIp(ip, "a28_e1");
    assertEq(H().ipFailedCount(ip), 0);
    H().clearFailedAttemptsIp(ip);
    H().bumpFailedAttemptIp(ip, "a28_e2");
    H().creditSuccessIp(ip, "a28_e1");
    assertEq(H().ipFailedCount(ip), 1, "e1 has no share in the new count");
});

console.log("totp audit 28: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
