// test_email.js — Tests for hull:email
//
// Tests field validation and provider dispatch (no network I/O).
// email.send follows the stdlib error convention: it THROWS an Error with a
// stable .code on failure and resolves to true on success.

import { email } from "hull:email";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        print("FAIL: " + name + ": " + (e && e.message));
    }
}

// Assert email.send(opts) throws a coded Error whose code === wantCode and
// (optionally) whose message contains wantMsg.
async function expectCode(name, opts, wantCode, wantMsg) {
    await test(name, async () => {
        let threw = null;
        try { await email.send(opts); }
        catch (e) { threw = e; }
        if (!threw) throw new Error("expected a throw");
        if (threw.code !== wantCode)
            throw new Error("expected code " + wantCode + ", got " + threw.code);
        if (wantMsg && String(threw).indexOf(wantMsg) === -1)
            throw new Error("expected message to contain '" + wantMsg + "', got: " + threw);
    });
}

await (async () => {
    // ── validation ──────────────────────────────────────────────────
    await expectCode("undefined opts throws", undefined, "invalid_argument", "opts required");
    await expectCode("null opts throws", null, "invalid_argument", "opts required");
    await expectCode("missing from throws",
        { to: "x@y.com", subject: "s", body: "b" }, "invalid_argument", "from required");
    await expectCode("missing to throws",
        { from: "x@y.com", subject: "s", body: "b" }, "invalid_argument", "to required");
    await expectCode("missing subject throws",
        { from: "x@y.com", to: "y@z.com", body: "b" }, "invalid_argument", "subject required");
    await expectCode("missing body throws",
        { from: "x@y.com", to: "y@z.com", subject: "s" }, "invalid_argument", "body required");
    await expectCode("invalid from address throws",
        { from: "bad", to: "y@z.com", subject: "s", body: "b" }, "invalid_argument", "invalid from address");
    await expectCode("invalid to address throws",
        { from: "x@y.com", to: "bad", subject: "s", body: "b" }, "invalid_argument", "invalid to address");

    // ── provider dispatch ───────────────────────────────────────────
    await expectCode("unknown provider throws",
        { provider: "unknown", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" },
        "unknown_provider", "unknown provider");

    // ── api provider validation ─────────────────────────────────────
    await expectCode("postmark requires api_key",
        { provider: "postmark", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" },
        "invalid_argument", "api_key required");
    await expectCode("sendgrid requires api_key",
        { provider: "sendgrid", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" },
        "invalid_argument", "api_key required");
    await expectCode("resend requires api_key",
        { provider: "resend", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" },
        "invalid_argument", "api_key required");

    // ── results ─────────────────────────────────────────────────────
    print(pass + " passed, " + fail + " failed");
    if (fail > 0) throw new Error(fail + " test(s) failed");
})();
