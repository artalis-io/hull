// test_stdlib_audit16.js - stdlib regressions from audit 16 (JS stdlib).
//
// A module's test hooks are no longer exported to app code: `inbox._test`
// un-marked any processed message and `totp._test.getBlob` returned any
// user's stored secret, from a stdlib frame the _hull_* check admits. They
// live in the stdlib-internal hull:_test_hooks, which this script (app code,
// by its module name) cannot import; the harness reads them through
// __testHooks. totp.init's allowUnboundSecrets: false refuses a secret sealed
// before audit 15 (unbound: it opens for any account), and rekeyStatus()
// counts such rows. 42.0 and 42 seal under one context (the Lua twin now
// agrees).
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db.

import { inbox } from "hull:web:middleware:inbox";
import { totp } from "hull:web:middleware:totp";
import { oauth } from "hull:web:middleware:oauth";
import { auditLog } from "hull:web:middleware:audit-log";
import { authFlows } from "hull:web:auth-flows";
import { sealbox } from "hull:crypto:sealbox";

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

// ── Test hooks are not app-reachable ───────────────────────────────

await test("no stdlib module exports _test", () => {
    const mods = { inbox, totp, oauth, auditLog, authFlows };
    for (const name of Object.keys(mods)) {
        assertEq(mods[name]._test, undefined, name);
    }
});

await test("app code cannot import hull:_test_hooks", async () => {
    let err = null;
    try {
        await import("hull:_test_hooks");
    } catch (e) {
        err = String(e && e.message || e);
    }
    assertEq(err !== null, true, "refused");
    assertEq(err.indexOf("internal to the Hull stdlib") >= 0, true, err);
});

// ── TOTP ───────────────────────────────────────────────────────────

const T = __testHooks("hull:web:middleware:totp");
const KEY = "k".repeat(32);
const RING = sealbox.keyring({ keys: { 1: KEY }, current: 1 });

// Enroll and confirm `uid`; returns [the raw secret, the confirmed step].
function enrolled(uid) {
    const r = totp.enroll(uid);
    const secret = T.base32Decode(r.secretBase32);
    const step = T.currentStep();
    assertEq(totp.confirm(uid, T.totpAtStep(secret, step, 6)), true, "confirm " + uid);
    return [secret, step];
}

await test("rekeyStatus counts unbound rows; a verify re-seals one bound", () => {
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
    const before = totp.rekeyStatus().unbound;
    const [secret, step] = enrolled("u20");
    T.setBlob("u20", sealbox.seal(RING, secret));   // sealed before audit 15
    assertEq(totp.rekeyStatus().unbound, before + 1, "counted");
    assertEq(totp.verify("u20", T.totpAtStep(secret, step + 1, 6)), true,
             "opens while allowed");
    assertEq(totp.rekeyStatus().unbound, before, "re-sealed bound");
});

await test("allowUnboundSecrets: false refuses an unbound secret", () => {
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
    const [secret, step] = enrolled("u21");
    T.setBlob("u21", sealbox.seal(RING, secret));
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY, allowUnboundSecrets: false });
    const unbound = totp.rekeyStatus().unbound;
    assertEq(unbound >= 1, true, "still counted when refused");
    assertEq(totp.verify("u21", T.totpAtStep(secret, step + 1, 6)), false, "refused");
    assertEq(totp.rekeyStatus().unbound, unbound, "not re-sealed");
    // A bound secret is unaffected.
    const [s2, st2] = enrolled("u22");
    assertEq(totp.verify("u22", T.totpAtStep(s2, st2 + 1, 6)), true, "bound still works");
    T.reset();
    let threw = false;
    try { totp.init({ encryptionKey: KEY, allowUnboundSecrets: "no" }); } catch (_) { threw = true; }
    assertEq(threw, true, "a non-boolean is refused");
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
});

await test("42.0 and 42 are one user id", () => {
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
    const r = totp.enroll(42);
    const secret = T.base32Decode(r.secretBase32);
    const step = T.currentStep();
    assertEq(totp.confirm(42.0, T.totpAtStep(secret, step, 6)), true, "confirm as 42.0");
    assertEq(totp.verify(42.0, T.totpAtStep(secret, step + 1, 6)), true, "verify as 42.0");
    const enc = T.encryptSecret(secret, 42.0);
    assertEq(T.decryptSecret(enc[0], 1, "42")[0], secret, "42.0 sealed as 42");
    assertEq(totp.verify(42.5, "123456"), false, "42.5 is no id");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
