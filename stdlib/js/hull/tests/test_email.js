// test_email.js - Tests for hull:email
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
        console.log("FAIL: " + name + ": " + (e && e.message));
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
    // One address only (audit 5): a comma-joined second recipient is refused.
    await expectCode("comma-joined to address throws",
        { from: "x@y.com", to: "a@x.co,b", subject: "s", body: "b" }, "invalid_argument", "invalid to address");
    await expectCode("angle-bracket to address throws",
        { from: "x@y.com", to: "A <a@x.co>", subject: "s", body: "b" }, "invalid_argument", "invalid to address");

    await expectCode("over-long to address throws",
        { from: "x@y.com", to: "a@" + "b".repeat(300) + ".com", subject: "s", body: "b" },
        "invalid_argument", "invalid to address");
    await expectCode("invalid cc address throws",
        { from: "x@y.com", to: "y@z.com", cc: ["ok@z.com", "bad"], subject: "s", body: "b" },
        "invalid_argument", "invalid cc address");

    // Reply-To is held to the same one-address rule (audit 10).
    await expectCode("header-injecting reply_to throws",
        { provider: "postmark", from: "x@y.com", to: "y@z.com",
          reply_to: "a@x.co\r\nBcc: v@w.co", subject: "s", body: "b" },
        "invalid_argument", "invalid reply_to address");
    await expectCode("comma-joined reply_to throws",
        { from: "x@y.com", to: "y@z.com", reply_to: "a@x.co,b@y.co", subject: "s", body: "b" },
        "invalid_argument", "invalid reply_to address");
    await expectCode("valid reply_to passes validation (fails later on api_key)",
        { provider: "postmark", from: "a@b.com", to: "c@d.com", reply_to: "r@s.com",
          subject: "s", body: "b" },
        "invalid_argument", "api_key required");
    // ── provider dispatch ───────────────────────────────────────────
    await expectCode("unknown provider throws",
        { provider: "unknown", from: "a@b.com", to: "c@d.com", subject: "s", body: "b" },
        "unknown_provider", "unknown provider");
    // An inherited Object.prototype name is not a provider (it "sent" nothing
    // and resolved).
    for (const p of ["toString", "constructor", "__proto__", "hasOwnProperty"]) {
        await expectCode("prototype name '" + p + "' is not a provider",
            { provider: p, from: "a@b.com", to: "c@d.com", subject: "s", body: "b" },
            "unknown_provider", "unknown provider");
    }

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
    console.log(pass + " passed, " + fail + " failed");
    // Published from INSIDE the async IIFE, after every await has settled --
    // the harness reads these once the module's evaluation promise resolves.
    // Every case here is a validation rejection (the file does no network
    // I/O), so they settle on the microtask queue.
    globalThis.__test_pass = pass;
    globalThis.__test_fail = fail;
})();
