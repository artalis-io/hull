// test_stdlib_audit22.js - stdlib regressions from audit 22 (JS stdlib).
//
// With keys configured, a plaintext row (encrypted = 0) - one a
// writer of _hull_totp planted, say - verified and was then sealed bound to
// the account. allowPlaintextSecrets: false refuses it.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): everything here needs the db.

import { totp } from "hull:web:middleware:totp";

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

const T = __testHooks("hull:web:middleware:totp");
const KEY = "k".repeat(32);

// Enroll and confirm `uid`; returns [the raw secret, the confirmed step].
function enrolled(uid) {
    const r = totp.enroll(uid);
    const secret = T.base32Decode(r.secretBase32);
    const step = T.currentStep();
    assertEq(totp.confirm(uid, T.totpAtStep(secret, step, 6)), true, "confirm " + uid);
    return [secret, step];
}

await test("allowPlaintextSecrets: false refuses a plaintext row", () => {
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
    const [secret, step] = enrolled("u23");
    T.setBlob("u23", secret, 0);
    assertEq(totp.rekeyStatus().plaintext, 1, "counted");
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY, allowPlaintextSecrets: false });
    assertEq(totp.verify("u23", T.totpAtStep(secret, step + 1, 6)), false, "refused");
    assertEq(totp.rekeyStatus().plaintext, 1, "not re-sealed");
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
    assertEq(totp.verify("u23", T.totpAtStep(secret, step + 1, 6)), true,
             "the default still lets it verify");
    assertEq(totp.rekeyStatus().plaintext, 0, "and seals it");
    let threw = false;
    try { totp.init({ encryptionKey: KEY, allowPlaintextSecrets: 1 }); } catch (_) { threw = true; }
    assertEq(threw, true, "a non-boolean is refused");
    T.reset(); totp.init({ issuer: "T", encryptionKey: KEY });
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
