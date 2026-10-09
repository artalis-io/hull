// test_auth_audit10.js - auth stdlib regressions from audit 10 (JS stdlib).
//
// auth-flows: while a confirmed email change can still be undone, its old
// address counts as taken (register, magic-link auto-signup, another
// account's change request and confirm), so the undo still has an address to
// restore; the undo whose address was taken anyway still makes the password
// unusable and pauses changes; the undo removes a second factor through
// totpDisable; magic-link auto-signup creates the account after the
// response. Plus the logout provenance origins auth-flows registers, the
// idempotency principal normalization and the inbox source check.
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): auth-flows and session need the db.

import { authFlows } from "hull:web:auth-flows";
import { cookie } from "hull:web:cookie";
import { session } from "hull:web:middleware:session";
import { idempotency } from "hull:web:middleware:idempotency";
import { inbox } from "hull:web:middleware:inbox";
import { crypto } from "hull:crypto";

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

function threw(fn) {
    try { fn(); return false; } catch (_e) { return true; }
}

// ── Fakes ──────────────────────────────────────────────────────────

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

function mkReq(body, ctx, headers) {
    const h = Object.assign({ host: "app.test", "content-type": "application/json",
                              "sec-fetch-site": "same-origin" }, headers || {});
    return { method: "POST", path: "/auth/x", headers: h, body: JSON.stringify(body || {}),
             ctx: ctx || {}, remote_addr: "192.0.2.7" };
}

let S;
function resetStore() {
    S = { byId: new Map(), byEmail: new Map(), sent: [], resets: 0, totpOff: [] };
}
function addUser(id, email, pw, verified) {
    const u = { id, email, email_verified: verified,
                password_hash: pw ? crypto.hashPassword(pw) : null };
    S.byId.set(String(id), u);
    S.byEmail.set(email, u);
    return u;
}

function init(extra) {
    authFlows._test.reset();
    authFlows.init(Object.assign({
        secret: "s".repeat(32),
        trustRequestHost: true,
        emailRateLimit: false,
        emailSend: (to, subject, _html, text) => { S.sent.push({ to, subject, text }); },
        templates: {
            welcome:        (c) => ({ subject: "w", text: c.verify_url }),
            magic_link:     (c) => ({ subject: "m", text: c.link }),
            password_reset: (c) => ({ subject: "p", text: c.link }),
            email_change:   (c) => ({ subject: "e", text: c.link }),
            email_change_notify: (c) => ({ subject: "n", text: c.revoke_url }),
        },
        userFindByEmail: (e) => S.byEmail.get(e) || null,
        userGet: (id) => S.byId.get(String(id)) || null,
        userCreate: (e, h) => {
            let id = 100;
            while (S.byId.has(String(id))) id++;
            const u = { id, email: e, password_hash: h, email_verified: false };
            S.byId.set(String(id), u); S.byEmail.set(e, u);
            return id;
        },
        userSetPassword: (id, h) => { S.byId.get(String(id)).password_hash = h; },
        userSetEmail: (id, e) => {
            const u = S.byId.get(String(id));
            S.byEmail.delete(u.email);
            u.email = e;
            S.byEmail.set(e, u);
        },
        userSetEmailVerified: (id, v) => { S.byId.get(String(id)).email_verified = v; },
        onLogin: (_req, res) => { res.json({ ok: true }); },
        onPasswordReset: () => { S.resets++; },
        emailChangeReauth: () => true,
    }, extra || {}));
}

const H = authFlows._test.handlers;

function tokenIn(to) {
    for (let i = S.sent.length - 1; i >= 0; i--) {
        if (S.sent[i].to === to) {
            const m = /token=(.+)$/.exec(S.sent[i].text);
            return m ? m[1] : null;
        }
    }
    return null;
}
function sentTo(to) { return S.sent.filter((m) => m.to === to).length; }

// A confirmed change of account id from oldEmail to newEmail; returns the
// revoke token.
async function confirmedChange(id, oldEmail, newEmail) {
    let res = mkRes();
    H.emailChange(mkReq({ new_email: newEmail }, { user_id: id }), res);
    assertEq(res.body && res.body.ok, true, "email-change start");
    const ctok = tokenIn(newEmail), rtok = tokenIn(oldEmail);
    res = mkRes();
    await H.emailChangeConfirm(mkReq({ token: ctok }), res);
    assertEq(res.code, 200, "confirm");
    return rtok;
}

// ── auth-flows: the vacated address is reserved ────────────────────

await test("register of a vacated old address creates nothing; the undo restores it", async () => {
    resetStore(); init();
    const u = addUser(5, "old5@x.test", "first-password-1", true);
    const rtok = await confirmedChange(5, "old5@x.test", "new5@x.test");
    S.sent = [];
    let res = mkRes();
    await H.register(mkReq({ email: "OLD5@x.test", password: "squatter-pw-1" }), res);
    assertEq(res.body && res.body.ok, true, "same ok answer (case-insensitive)");
    res = mkRes();
    await H.register(mkReq({ email: "old5@x.test", password: "squatter-pw-1" }), res);
    assertEq(res.body && res.body.ok, true, "same ok answer");
    assertEq(S.byEmail.has("old5@x.test"), false, "no account created");
    assertEq(S.byId.size, 1, "one account");
    assertEq(S.sent.length, 0, "no welcome mail");
    res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.body && res.body.restored, true, "restored");
    assertEq(u.email, "old5@x.test");
});

await test("magic-link auto-signup of a vacated address creates nothing", async () => {
    resetStore(); init({ magicLinkAutoSignup: true });
    addUser(6, "old6@x.test", "first-password-1", true);
    await confirmedChange(6, "old6@x.test", "new6@x.test");
    S.sent = [];
    const res = mkRes();
    H.magicLink(mkReq({ email: "old6@x.test" }), res);
    assertEq(res.body && res.body.ok, true);
    assertEq(S.byEmail.has("old6@x.test"), false, "no account created");
    assertEq(S.sent.length, 0, "no mail");
    authFlows.sendMagicLink("old6@x.test", "https://app.test");
    assertEq(S.byEmail.has("old6@x.test"), false, "sendMagicLink creates nothing either");
});

await test("magic-link auto-signup creates a new account (after the response)", () => {
    resetStore(); init({ magicLinkAutoSignup: true });
    const res = mkRes();
    H.magicLink(mkReq({ email: "fresh@x.test" }), res);
    assertEq(res.body && res.body.ok, true);
    const u = S.byEmail.get("fresh@x.test");
    assertEq(typeof u, "object", "account created");
    assertEq(u.password_hash, null, "passwordless");
    assertEq(sentTo("fresh@x.test"), 1, "magic link mailed");
    H.magicLink(mkReq({ email: "fresh@x.test" }), mkRes());
    assertEq(S.byId.size, 1);
});

await test("another account cannot request a change to the vacated address", async () => {
    resetStore(); init();
    addUser(7, "old7@x.test", "first-password-1", true);
    const other = addUser(8, "b8@x.test", "other-password-1", true);
    await confirmedChange(7, "old7@x.test", "new7@x.test");
    const res = mkRes();
    H.emailChange(mkReq({ new_email: "old7@x.test" }, { user_id: 8 }), res);
    assertEq(res.code, 409, "the vacated address is reserved");
    assertEq(other.email, "b8@x.test");
});

await test("a confirm link to the vacated address answers 409", async () => {
    resetStore(); init();
    const nine = addUser(9, "old9@x.test", "first-password-1", true);
    const other = addUser(10, "b10@x.test", "other-password-1", true);
    // Account 10 asks for c9@x.test while it is free; account 9 then takes
    // it and moves on, leaving it vacated - and reserved for 9's undo.
    let res = mkRes();
    H.emailChange(mkReq({ new_email: "c9@x.test" }, { user_id: 10 }), res);
    assertEq(res.body && res.body.ok, true);
    const ctok10 = tokenIn("c9@x.test");
    S.byEmail.delete("old9@x.test");
    nine.email = "c9@x.test";
    S.byEmail.set("c9@x.test", nine);
    await confirmedChange(9, "c9@x.test", "new9@x.test");
    assertEq(S.byEmail.has("c9@x.test"), false, "c9 vacated");
    res = mkRes();
    await H.emailChangeConfirm(mkReq({ token: ctok10 }), res);
    assertEq(res.code, 409, "reserved for the undo");
    assertEq(other.email, "b10@x.test");
});

await test("undo whose address was taken anyway still resets the password and pauses changes", async () => {
    resetStore(); init();
    const u = addUser(11, "old11@x.test", "first-password-1", true);
    const oldHash = u.password_hash;
    const rtok = await confirmedChange(11, "old11@x.test", "new11@x.test");
    addUser(12, "old11@x.test", "squatter-pw-1", true);   // the app's own create
    let res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.code, 409, "nothing to restore to");
    assertEq(u.email, "new11@x.test");
    assertEq(u.password_hash !== oldHash, true, "password replaced");
    assertEq(crypto.verifyPassword("first-password-1", u.password_hash), false);
    assertEq(S.resets >= 1, true, "sessions revoked");
    res = mkRes();
    H.emailChange(mkReq({ new_email: "again11@x.test" }, { user_id: 11 }), res);
    assertEq(res.code, 409, "paused");
});

await test("undo of a confirmed change removes the second factor via totpDisable", async () => {
    resetStore();
    init({ totpDisable: (id) => { S.totpOff.push(id); } });
    addUser(13, "old13@x.test", "first-password-1", true);
    const rtok = await confirmedChange(13, "old13@x.test", "new13@x.test");
    const res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.body && res.body.restored, true);
    assertEq(S.totpOff.length, 1, "totpDisable called");
    assertEq(String(S.totpOff[0]), "13");
});

// ── logout provenance ──────────────────────────────────────────────

await test("session logout trusts the origins auth-flows registered", () => {
    resetStore();
    init({ trustRequestHost: false, publicOrigin: "https://app.example.com" });
    session.init({ cleanup: false });
    const lo = session.logoutHandler(cookie);
    const run = (headers) => {
        const res = mkRes();
        lo({ method: "POST", headers }, res);
        return res.code;
    };
    // Behind a proxy: Host is the upstream name, Origin the public one.
    assertEq(run({ host: "10.0.0.5:8080", origin: "https://app.example.com" }), 200);
    assertEq(run({ host: "10.0.0.5:8080", origin: "https://evil.test" }), 403);
    const lo2 = session.logoutHandler(cookie, { trustedHosts: ["alt.example.com"] });
    const res = mkRes();
    lo2({ method: "POST", headers: { host: "10.0.0.5", origin: "https://alt.example.com:8443" } }, res);
    assertEq(res.code, 200, "opts.trustedHosts, any port");
    authFlows._test.reset();
    assertEq(run({ host: "10.0.0.5:8080", origin: "https://app.example.com" }), 403,
             "cleared with the auth-flows state");
});

// ── idempotency ────────────────────────────────────────────────────

await test("idempotency: principals are text, capped at the column width", () => {
    const n = idempotency._normPrincipal;
    assertEq(n(5), "5");
    assertEq(n(null), "__anon");
    assertEq(n(""), "__anon");
    assertEq(n("user:5"), "user:5");
    const long = "a".repeat(300);
    const h = n(long);
    assertEq(h.length <= 255, true);
    assertEq(h.slice(0, 7), "sha256:");
    assertEq(n(long + "b") !== h, true, "distinct long principals stay distinct");
    assertEq(n(h) !== h, true, "a literal hashed spelling cannot pose as one");
    assertEq(threw(() => n({})), true, "an object is refused");
    assertEq(threw(() => n(Promise.resolve("x"))), true, "a Promise is refused");
});

// ── inbox ──────────────────────────────────────────────────────────

await test("inbox: an integer source is its decimal string; other types refused", () => {
    inbox.init({ ttl: 60 });
    assertEq(inbox.checkAndMark("m1", 42), false);
    assertEq(inbox.checkAndMark("m1", "42"), true, "42 and \"42\" are one source");
    assertEq(threw(() => inbox.isDuplicate("m1", {})), true);
    assertEq(threw(() => inbox.isDuplicate("m1", "s".repeat(256))), true, "over-length");
});

// ── audit 11 ───────────────────────────────────────────────────────

await test("audit 11: an address unverified at confirm is not reserved", async () => {
    resetStore(); init();
    addUser(20, "old20@x.test", "first-password-1", false);
    await confirmedChange(20, "old20@x.test", "new20@x.test");
    assertEq(authFlows.emailReserved("old20@x.test"), false);
    await H.register(mkReq({ email: "old20@x.test", password: "owner-password-1" }), mkRes());
    assertEq(!!S.byEmail.get("old20@x.test"), true, "the owner can register it");
});

await test("audit 11: emailReserved reports an address held for an undo", async () => {
    resetStore(); init();
    addUser(21, "old21@x.test", "first-password-1", true);
    await confirmedChange(21, "old21@x.test", "new21@x.test");
    assertEq(authFlows.emailReserved("OLD21@x.test"), true);
    assertEq(authFlows.emailReserved("other21@x.test"), false);
});

await test("audit 11: an undo that cannot restore locks recovery and drops the second factor", async () => {
    resetStore();
    init({ totpDisable: (id) => { S.totpOff.push(id); } });
    addUser(22, "old22@x.test", "first-password-1", true);
    const rtok = await confirmedChange(22, "old22@x.test", "new22@x.test");
    H.magicLink(mkReq({ email: "new22@x.test" }), mkRes());
    const mtok = tokenIn("new22@x.test");
    assertEq(typeof mtok, "string", "magic link issued");
    addUser(23, "old22@x.test", "squatter-pw-1", true);
    let res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.code, 409);
    assertEq(S.totpOff.length, 1, "totpDisable called");
    S.sent = [];
    H.passwordResetRequest(mkReq({ email: "new22@x.test" }), mkRes());
    H.magicLink(mkReq({ email: "new22@x.test" }), mkRes());
    authFlows.sendPasswordReset("new22@x.test", "https://app.test");
    authFlows.sendMagicLink("new22@x.test", "https://app.test");
    assertEq(sentTo("new22@x.test"), 0, "no reset or magic link while locked");
    res = mkRes();
    await H.magicLinkConsume(mkReq({ token: mtok }), res);
    assertEq(res.code, 400, "an earlier magic link no longer signs in");
});

await test("audit 11: idempotency skips a request a custom getPrincipal cannot place", () => {
    for (const p of [null, undefined, false, ""]) {
        const mw = idempotency.middleware({ getPrincipal: () => p });
        const req = { method: "POST", path: "/x", body: "{}", ctx: {},
                      header: (n) => (n === "idempotency-key" ? "k1" : null) };
        assertEq(mw(req, mkRes()), 0, "runs without idempotency: " + String(p));
    }
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
