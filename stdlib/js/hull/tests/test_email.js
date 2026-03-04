// test_email.js — Tests for hull:email
//
// Tests field validation and provider dispatch (no network I/O).

import email from "hull:email";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        print("FAIL: " + name + ": " + e.message);
    }
}

function assertEq(a, b, msg) {
    if (a !== b)
        throw new Error((msg || "") + " expected " + b + ", got " + a);
}

// ── validation ──────────────────────────────────────────────────────

test("undefined opts returns error", () => {
    const r = email.send(undefined);
    assertEq(r.ok, false);
    assertEq(r.error, "opts required");
});

test("null opts returns error", () => {
    const r = email.send(null);
    assertEq(r.ok, false);
    assertEq(r.error, "opts required");
});

test("missing from returns error", () => {
    const r = email.send({ to: "x@y.com", subject: "s", body: "b" });
    assertEq(r.ok, false);
    assertEq(r.error, "from required");
});

test("missing to returns error", () => {
    const r = email.send({ from: "x@y.com", subject: "s", body: "b" });
    assertEq(r.ok, false);
    assertEq(r.error, "to required");
});

test("missing subject returns error", () => {
    const r = email.send({ from: "x@y.com", to: "y@z.com", body: "b" });
    assertEq(r.ok, false);
    assertEq(r.error, "subject required");
});

test("missing body returns error", () => {
    const r = email.send({ from: "x@y.com", to: "y@z.com", subject: "s" });
    assertEq(r.ok, false);
    assertEq(r.error, "body required");
});

// ── provider dispatch ───────────────────────────────────────────────

test("unknown provider returns error", () => {
    const r = email.send({
        provider: "unknown",
        from: "a@b.com", to: "c@d.com",
        subject: "s", body: "b",
    });
    assertEq(r.ok, false);
    if (r.error.indexOf("unknown provider") === -1)
        throw new Error("expected 'unknown provider' in error: " + r.error);
});

test("default provider is smtp", () => {
    // Will fail because smtp is not configured in test env, but should not
    // error about "unknown provider"
    const r = email.send({
        from: "a@b.com", to: "c@d.com",
        subject: "s", body: "b",
        smtp_host: "localhost",
    });
    if (r.error && r.error.indexOf("unknown provider") !== -1)
        throw new Error("should dispatch to smtp, not fail with unknown provider");
});

// ── api provider validation ─────────────────────────────────────────

test("postmark requires api_key", () => {
    const r = email.send({
        provider: "postmark",
        from: "a@b.com", to: "c@d.com",
        subject: "s", body: "b",
    });
    assertEq(r.ok, false);
    if (r.error.indexOf("api_key required") === -1)
        throw new Error("expected api_key required error: " + r.error);
});

test("sendgrid requires api_key", () => {
    const r = email.send({
        provider: "sendgrid",
        from: "a@b.com", to: "c@d.com",
        subject: "s", body: "b",
    });
    assertEq(r.ok, false);
    if (r.error.indexOf("api_key required") === -1)
        throw new Error("expected api_key required error: " + r.error);
});

test("resend requires api_key", () => {
    const r = email.send({
        provider: "resend",
        from: "a@b.com", to: "c@d.com",
        subject: "s", body: "b",
    });
    assertEq(r.ok, false);
    if (r.error.indexOf("api_key required") === -1)
        throw new Error("expected api_key required error: " + r.error);
});

// ── results ─────────────────────────────────────────────────────────

print(pass + " passed, " + fail + " failed");
if (fail > 0) throw new Error(fail + " test(s) failed");
