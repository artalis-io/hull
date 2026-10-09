// test_web_audit9.js - web stdlib regressions from audit 9 (JS stdlib).
//
// auth-flows: an undone (confirmed, then revoked) email change restores the
// old address's verified state, makes the password unusable and pauses new
// changes (M-a, M-b); _hull_auth_* rows are keyed by the text form of the id
// (M-c); a user setter that returns a Promise is refused (M-d); a confirm
// whose new address was taken since answers 409 without consuming the link;
// a pending-2FA token dies with the password it was issued against;
// stripUserSecrets reads own allowlisted keys only. Plus ratelimit (a spent
// bucket survives eviction), cookie (first occurrence wins), session logout
// provenance, the idempotency default principal and the oauth tid check.
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): auth-flows and session need the db.

import { authFlows } from "hull:web:auth-flows";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { cache } from "hull:cache";
import { cookie } from "hull:web:cookie";
import { session } from "hull:web:middleware:session";
import { idempotency } from "hull:web:middleware:idempotency";
import { oauth } from "hull:web:middleware:oauth";
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

// In-memory users with INTEGER ids (the M-c shape), keyed by their text.
let S;
function resetStore() {
    S = { byId: new Map(), byEmail: new Map(), sent: [], resets: 0, logins: 0 };
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
        onLogin: (_req, res, user) => { S.logins++; res.json({ ok: true, id: user.id }); },
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

function startChange(id, oldEmail, newEmail) {
    const res = mkRes();
    H.emailChange(mkReq({ new_email: newEmail }, { user_id: id }), res);
    assertEq(res.body && res.body.ok, true, "email-change start");
    return [tokenIn(newEmail), tokenIn(oldEmail)];
}

// ── auth-flows ─────────────────────────────────────────────────────

await test("uidKey: integer ids are keyed as text", () => {
    assertEq(authFlows._test.uidKey(5), "5");
    assertEq(authFlows._test.uidKey("u1"), "u1");
});

await test("undo of a confirmed change: unverified old address stays unverified, "
           + "password unusable, new changes paused (M-a, M-b)", async () => {
    resetStore(); init();
    const u = addUser(5, "old5@x.test", "first-password-1", false);
    const oldHash = u.password_hash;
    const [ctok, rtok] = startChange(5, "old5@x.test", "new5@x.test");
    let res = mkRes();
    await H.emailChangeConfirm(mkReq({ token: ctok }), res);
    assertEq(res.code, 200, "confirm");
    assertEq(u.email, "new5@x.test");
    assertEq(u.email_verified, true);
    res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.body && res.body.restored, true, "restored");
    assertEq(res.body.password_reset_required, true);
    assertEq(u.email, "old5@x.test");
    assertEq(u.email_verified, false, "the old address was not verified");
    assertEq(u.password_hash !== oldHash, true, "password replaced");
    assertEq(crypto.verifyPassword("first-password-1", u.password_hash), false,
             "the old password no longer works");
    assertEq(S.resets >= 1, true, "sessions revoked");
    res = mkRes();
    H.emailChange(mkReq({ new_email: "again5@x.test" }, { user_id: 5 }), res);
    assertEq(res.code, 409, "paused");
    assertEq(String(res.body.error).indexOf("paused") >= 0, true);
});

await test("undo of a confirmed change: a verified old address stays verified (M-a)", async () => {
    resetStore(); init();
    const u = addUser(6, "old6@x.test", "first-password-1", true);
    const [ctok, rtok] = startChange(6, "old6@x.test", "new6@x.test");
    await H.emailChangeConfirm(mkReq({ token: ctok }), mkRes());
    const res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.body.restored, true);
    assertEq(u.email, "old6@x.test");
    assertEq(u.email_verified, true);
});

await test("confirm re-checks that the new address is still free (409, link kept)", async () => {
    resetStore(); init();
    const u = addUser(7, "old7@x.test", "first-password-1", true);
    const [ctok] = startChange(7, "old7@x.test", "taken7@x.test");
    addUser(70, "taken7@x.test", "other-password-1", false);
    let res = mkRes();
    await H.emailChangeConfirm(mkReq({ token: ctok }), res);
    assertEq(res.code, 409, "address in use");
    assertEq(u.email, "old7@x.test");
    S.byEmail.delete("taken7@x.test");
    S.byId.delete("70");
    res = mkRes();
    await H.emailChangeConfirm(mkReq({ token: ctok }), res);
    assertEq(res.code, 200, "confirm after the address was freed");
    assertEq(u.email, "taken7@x.test");
});

await test("a user setter returning a Promise is refused (M-d)", async () => {
    resetStore();
    init({ userSetEmail: async () => {} });
    addUser(8, "old8@x.test", "first-password-1", true);
    const [ctok] = startChange(8, "old8@x.test", "new8@x.test");
    let refused = false;
    try { await H.emailChangeConfirm(mkReq({ token: ctok }), mkRes()); }
    catch (e) { refused = /must be synchronous/.test(String(e && e.message)); }
    assertEq(refused, true);
});

await test("a pending-2FA token dies with the password it was issued against", async () => {
    resetStore();
    init({ enableTotp: true, totpDisable: () => {}, userTotpEnrolled: () => true, totpVerify: () => true });
    const u = addUser(9, "u9@x.test", "first-password-1", true);
    let res = mkRes();
    await H.login(mkReq({ email: "u9@x.test", password: "first-password-1" }), res);
    let tok = res.body && res.body.totp_token;
    assertEq(typeof tok, "string", "pending token");
    u.password_hash = crypto.hashPassword("second-password-2");
    res = mkRes();
    await H.totpVerify(mkReq({ token: tok, code: "123456" }), res);
    assertEq(res.code, 400, "old pending token refused");
    assertEq(S.logins, 0);
    res = mkRes();
    await H.login(mkReq({ email: "u9@x.test", password: "second-password-2" }), res);
    tok = res.body.totp_token;
    res = mkRes();
    await H.totpVerify(mkReq({ token: tok, code: "123456" }), res);
    assertEq(res.body && res.body.ok, true, "fresh token works");
    assertEq(S.logins, 1);
});

await test("stripUserSecrets: own allowlisted keys only", () => {
    resetStore(); init();
    const proto = { id: "inherited", password_hash: "h" };
    const u = Object.assign(Object.create(proto), {
        email: "a@x.test", constructor: "secret", toString: "secret2", totp_secret: "t" });
    const out = authFlows._test.stripUserSecrets(u);
    assertEq(Object.keys(out).join(","), "email");
});

// ── ratelimit ──────────────────────────────────────────────────────

await test("ratelimit: a spent bucket (count == limit) survives eviction", () => {
    const buckets = cache.new({ maxEntries: 2 });
    const sat = new Map();
    const now = 1000;
    assertEq(ratelimit.check(buckets, "A", 2, 60, now, sat).allowed, true);
    assertEq(ratelimit.check(buckets, "A", 2, 60, now, sat).allowed, true);
    ratelimit.check(buckets, "B", 2, 60, now, sat);
    ratelimit.check(buckets, "C", 2, 60, now, sat);
    assertEq(ratelimit.check(buckets, "A", 2, 60, now, sat).allowed, false,
             "A's spent allowance came back after eviction");
});

// ── cookie ─────────────────────────────────────────────────────────

await test("cookie: first occurrence wins; values decode as in Lua", () => {
    const c = cookie.parse('sid=first; sid=second; q="x%20y"; bad=%zz');
    assertEq(c.sid, "first");
    assertEq(c.q, "x y");
    assertEq(c.bad, "%zz");
});

// ── session logout ─────────────────────────────────────────────────

session.init({ cleanup: false });

await test("session.logoutHandler: same-site and foreign Origin refused", () => {
    const lo = session.logoutHandler(cookie);
    const run = (headers) => {
        const res = mkRes();
        lo({ method: "POST", headers }, res);
        return res.code;
    };
    assertEq(run({ host: "app.test", "sec-fetch-site": "same-site" }), 403);
    assertEq(run({ host: "app.test", "sec-fetch-site": "cross-site" }), 403);
    assertEq(run({ host: "app.test", origin: "https://evil.test" }), 403);
    assertEq(run({ host: "app.test", referer: "https://evil.test/x" }), 403);
    assertEq(run({ host: "app.test", origin: "null" }), 403);
    assertEq(run({ host: "app.test", "sec-fetch-site": "same-origin" }), 200);
    assertEq(run({ host: "app.test", origin: "http://app.test" }), 200);
    assertEq(run({ host: "app.test" }), 200, "a header-less client passes");
});

// ── idempotency ────────────────────────────────────────────────────

await test("idempotency: session and JWT principals are prefixed apart", () => {
    const p = idempotency._defaultPrincipal;
    assertEq(p({ ctx: { session: { user_id: 5 } } }), "session:5");
    assertEq(p({ ctx: { user: { sub: 5 } } }), "user:5");
    assertEq(p({ ctx: { session: { user_id: "user:5" } } }) !== "user:5", true);
    assertEq(p({ ctx: {} }), "__anon");
});

// ── oauth ──────────────────────────────────────────────────────────

await test("oauth: a multi-tenant issuer must be the token's own tenant", () => {
    const ok = oauth._test.issuerOk;
    const common = oauth._test.presets.microsoft({ tenant: "common" });
    const iss = "https://login.microsoftonline.com/1111-aaaa/v2.0";
    assertEq(ok(common, { iss, tid: "1111-aaaa" }), true);
    assertEq(ok(common, { iss, tid: "2222-bbbb" }), false, "other tid");
    assertEq(ok(common, { iss }), false, "no tid");
    const orgs = oauth._test.presets.microsoft({ tenant: "organizations" });
    const msa = "https://login.microsoftonline.com/9188040d-6c67-4c5b-b112-36a304b66dad/v2.0";
    assertEq(ok(orgs, { iss: msa, tid: "9188040d-6c67-4c5b-b112-36a304b66dad" }), false);
    assertEq(ok({ issuer: "https://idp.test" }, { iss: "https://idp.test" }), true);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
