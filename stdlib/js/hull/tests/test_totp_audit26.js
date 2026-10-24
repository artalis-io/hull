// test_totp_audit26.js - TOTP regressions from audit 26 (JS stdlib).
//
// A secret opened without its account context - an unbound v2 blob or a
// legacy v1 one - must be exactly 20 bytes and not a bound frame, and the v1
// path is gated by allowUnboundSecrets like the unbound v2 one: a v2 blob
// with its 4-byte version stripped (nonce || box) read as v1 under the
// encryptionKey shorthand's legacy key, so another account's bound blob
// became this account's secret. A successful verify no longer clears the
// per-IP failure count. maxLockoutDuration and pendingTtl are numbers.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db.

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

const H = () => __testHooks("hull:web:middleware:totp");
const KEY = "k".repeat(32);

function be32(n) {
    return String.fromCharCode((n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255);
}

// Enroll + confirm a user; returns its raw secret.
function enroll(uid) {
    const r = totp.enroll(uid);
    const secret = H().base32Decode(r.secretBase32);
    const code = H().totpAtStep(secret, H().currentStep(), 6);
    assertEq(totp.confirm(uid, code), true, "confirm " + uid);
    return secret;
}

// ── Cross-account transplant through the legacy v1 path ────────────

test("totp: a stripped bound blob does not open as another account's v1 row", () => {
    H().reset();
    totp.init({ issuer: "T", encryptionKey: KEY, cleanup: false });
    enroll("attacker");
    enroll("victim");
    const blob = H().getBlob("attacker")[0];
    const stripped = blob.substring(4);   // drop the 4-byte version: nonce || box
    assertEq(H().decryptSecret(stripped, 1, "victim")[0], null,
             "a stripped bound blob is refused");
    // End to end: the attacker writes it into the victim's row and computes
    // codes from the frame it would have opened to.
    const ring = sealbox.keyring({ keys: { 1: KEY }, current: 1 });
    const frame = sealbox.openUnversioned(ring, 1, stripped);
    assertEq(typeof frame, "string", "the stripped blob is a v1-shaped box");
    H().setBlob("victim", stripped, 1);
    const step = H().currentStep();
    let ok = false;
    for (let off = -1; off <= 1; off++) {
        if (totp.verify("victim", H().totpAtStep(frame, step + off, 6))) ok = true;
    }
    assertEq(ok, false, "the transplanted frame is not the victim's secret");
});

test("totp: a stripped unbound blob is refused while allowUnboundSecrets is off", () => {
    H().reset();
    totp.init({ issuer: "T", encryptionKey: KEY, cleanup: false,
                allowUnboundSecrets: false });
    const ring = sealbox.keyring({ keys: { 1: KEY }, current: 1 });
    const secret = "S".repeat(20);
    const v1 = sealbox.seal(ring, secret).substring(4);   // a legacy v1 blob
    assertEq(H().decryptSecret(v1, 1, "u1")[0], null, "v1 is unbound: refused");
    // rekeyStatus still counts it (as unbound).
    const dec = H().decryptSecret(v1, 1, "u1", true);
    assertEq(dec[0], secret, "counted when allowed explicitly");
    assertEq(dec[1], 0); assertEq(dec[2], true);
});

test("totp: a legacy v1 row still opens while allowed, and only at 20 bytes", () => {
    H().reset();
    totp.init({ issuer: "T", encryptionKey: KEY, cleanup: false });
    const ring = sealbox.keyring({ keys: { 1: KEY }, current: 1 });
    const secret = "S".repeat(20);
    const dec = H().decryptSecret(sealbox.seal(ring, secret).substring(4), 1, "u1");
    assertEq(dec[0], secret, "a genuine v1 row opens");
    assertEq(dec[1], 0, "version 0 = legacy v1");
    assertEq(dec[2], true, "flagged for a bound re-seal");
    assertEq(H().decryptSecret(sealbox.seal(ring, "S".repeat(21)).substring(4), 1, "u1")[0],
             null, "a v1 plaintext of another length is refused");
    assertEq(H().decryptSecret(sealbox.seal(ring, "S".repeat(19)), 1, "u1")[0], null,
             "an unbound v2 plaintext of another length is refused");
    const boundFrame = be32(4) + "totp" + be32(1) + "x" + "S".repeat(10);
    assertEq(H().decryptSecret(sealbox.seal(ring, boundFrame).substring(4), 1, "u1")[0],
             null, "a v1 plaintext that is a bound frame is refused");
});

// ── The per-IP count survives a success ────────────────────────────

test("totp: a successful verify does not reset the per-IP failure count", () => {
    H().reset();
    totp.init({ issuer: "T", cleanup: false, maxFailedAttemptsPerIp: 3 });
    const sa = enroll("ip_a");
    enroll("ip_b");
    const req = { remote_addr: "203.0.113.9", headers: {} };
    assertEq(totp.verify("ip_b", "000000x", req), false);
    assertEq(totp.verify("ip_b", "000000y", req), false);
    // The attacker's own account, at a step confirm did not use.
    assertEq(totp.verify("ip_a", H().totpAtStep(sa, H().currentStep() + 1, 6), req),
             true, "own code verifies");
    assertEq(totp.verify("ip_b", "000000z", req), false);
    assertEq(H().lockoutRemainingIp("203.0.113.9") > 0, true,
             "the third failure locks the IP");
});

// ── Numeric options ────────────────────────────────────────────────

test("totp: maxLockoutDuration must be a finite number >= 0", () => {
    for (const bad of ["3600", -1, NaN, Infinity, true, {}]) {
        H().reset();
        raises(() => totp.init({ maxLockoutDuration: bad, cleanup: false }),
               "maxLockoutDuration", String(bad));
    }
    H().reset();
    totp.init({ maxLockoutDuration: 0, cleanup: false });
    H().reset();
    totp.init({ maxLockoutDuration: 3600, cleanup: false });
});

test("totp: pendingTtl must be a finite number or false", () => {
    for (const bad of ["3600", NaN, Infinity, -Infinity, true, {}]) {
        H().reset();
        raises(() => totp.init({ pendingTtl: bad, cleanup: false }),
               "pendingTtl", String(bad));
    }
    H().reset();
    totp.init({ pendingTtl: false, cleanup: false });
    H().reset();
    totp.init({ pendingTtl: 60, cleanup: false });
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
console.log("totp audit 26: " + pass + " passed, " + fail + " failed");
