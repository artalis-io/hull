// test_auth_audit12.js - auth-flows regressions from audit 12 (JS stdlib).
//
// Token mails (magic link, password reset, verify resend, and the
// sendPasswordReset / sendMagicLink helpers) go to the account's STORED
// address, and only when the typed address is that address (the domain may
// differ in ASCII case): an app lookup that folds case or accents no longer
// hands an account's token to whoever reads a lookalike mailbox; the bundled
// standardUsers lookup compares exactly. The email-change notice skips the
// per-recipient rate limit and a change whose notice cannot be sent (a throw,
// a rejection, false) is not started. The recovery lock of an undo that could
// not restore the old address no longer expires (unlockRecovery clears it).
// The revoke and the confirm act on the change's row only while it is still
// the state they read. init refuses email_change_notify without
// onPasswordReset and enableTotp without totpDisable.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): auth-flows needs the db.

import { authFlows } from "hull:web:auth-flows";
import { crypto } from "hull:crypto";
import { db as dbModule } from "hull:db";

const db = dbModule.default();

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

function mkReq(body, ctx) {
    return { method: "POST", path: "/auth/x",
             headers: { host: "app.test", "content-type": "application/json",
                        "sec-fetch-site": "same-origin" },
             body: JSON.stringify(body || {}), ctx: ctx || {}, remote_addr: "192.0.2.7" };
}

// The store's lookup FOLDS, as a MySQL utf8mb4_0900_ai_ci column does: case
// and the accents these tests use.
function fold(e) { return e.toLowerCase().replace(/é/g, "e"); }

let S;
function resetStore() {
    S = { byId: new Map(), byEmail: new Map(), sent: [], resets: 0, totpOff: [],
          order: [], failNotice: null, failSetEmail: null, onGet: null };
}
function addUser(id, email, pw, verified) {
    const u = { id, email, email_verified: verified,
                password_hash: pw ? crypto.hashPassword(pw) : null };
    S.byId.set(String(id), u);
    S.byEmail.set(fold(email), u);
    return u;
}

function init(extra) {
    authFlows._test.reset();
    authFlows.init(Object.assign({
        secret: "s".repeat(32),
        trustRequestHost: true,
        emailRateLimit: false,
        emailChangeTtl: 86400,
        emailSend: (to, subject, _html, text) => {
            if (subject === "n" && S.failNotice === "throw") throw new Error("smtp down");
            if (subject === "n" && S.failNotice === "false") return false;
            if (subject === "n" && S.failNotice === "reject") return Promise.reject(new Error("smtp down"));
            S.sent.push({ to, subject, text });
            return undefined;
        },
        templates: {
            welcome:        (c) => ({ subject: "w", text: c.verify_url }),
            magic_link:     (c) => ({ subject: "m", text: c.link }),
            password_reset: (c) => ({ subject: "p", text: c.link }),
            email_change:   (c) => ({ subject: "e", text: c.link }),
            email_change_notify: (c) => ({ subject: "n", text: c.revoke_url }),
        },
        userFindByEmail: (e) => S.byEmail.get(fold(e)) || null,
        userGet: (id) => {
            if (S.onGet) { const f = S.onGet; S.onGet = null; f(); }
            return S.byId.get(String(id)) || null;
        },
        userCreate: (e, h) => {
            let id = 100;
            while (S.byId.has(String(id))) id++;
            const u = { id, email: e, password_hash: h, email_verified: false };
            S.byId.set(String(id), u); S.byEmail.set(fold(e), u);
            return id;
        },
        userSetPassword: (id, h) => { S.byId.get(String(id)).password_hash = h; },
        userSetEmail: (id, e) => {
            if (S.failSetEmail === e) throw new Error("duplicate key");
            const u = S.byId.get(String(id));
            S.byEmail.delete(fold(u.email));
            u.email = e;
            S.byEmail.set(fold(e), u);
        },
        userSetEmailVerified: (id, v) => { S.byId.get(String(id)).email_verified = v; },
        onLogin: (_req, res) => { res.json({ ok: true }); },
        onPasswordReset: () => { S.resets++; S.order.push("sessions"); },
        totpDisable: async (id) => {
            S.totpOff.push(id);
            S.order.push("totp");
            S.lockedAtTotp = authFlows._test.recoveryLocked(id);
        },
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

async function startChange(id, newEmail) {
    const res = mkRes();
    await H.emailChange(mkReq({ new_email: newEmail }, { user_id: id }), res);
    return res;
}

async function confirmedChange(id, oldEmail, newEmail) {
    let res = await startChange(id, newEmail);
    assertEq(res.body && res.body.ok, true, "email-change start");
    const ctok = tokenIn(newEmail), rtok = tokenIn(oldEmail);
    res = mkRes();
    await H.emailChangeConfirm(mkReq({ token: ctok }), res);
    assertEq(res.code, 200, "confirm");
    return rtok;
}

// ── H1: token mails go to the stored address ──────────────────────

await test("sameAddress: equal after ASCII lowercasing, nothing more", () => {
    const sa = authFlows._test.sameAddress;
    assertEq(sa("jose@x.test", "jose@x.test"), true);
    assertEq(sa("jose@X.Test", "jose@x.test"), true, "domain case");
    assertEq(sa("JOSE@x.test", "jose@x.test"), true, "local-part ASCII case");
    assertEq(sa("josé@x.test", "jose@x.test"), false, "accent");
    assertEq(sa("jose@x.tést", "jose@x.test"), false, "non-ASCII domain");
    assertEq(sa("jose@Kelvin.test", "jose@kelvin.test"), false, "no Unicode case folding");
    assertEq(sa("jose@x.test", null), false, "no stored address");
});

await test("reset / magic link / helpers for a lookalike address mail nothing", async () => {
    resetStore(); init({ magicLinkAutoSignup: true });
    addUser(1, "jose@x.test", "first-password-1", true);
    for (const typed of ["josé@x.test", "JOSÉ@x.test", "jose@x.tést"]) {
        let res = mkRes();
        await H.passwordResetRequest(mkReq({ email: typed }), res);
        assertEq(res.body && res.body.ok, true, "generic ok");
        res = mkRes();
        await H.magicLink(mkReq({ email: typed }), res);
        assertEq(res.body && res.body.ok, true, "generic ok");
        authFlows.sendPasswordReset(typed, "https://app.test");
        authFlows.sendMagicLink(typed, "https://app.test");
    }
    assertEq(S.sent.length, 0, "no token mailed, to either address");
    assertEq(S.byId.size, 1, "no account auto-created for a lookalike");
});

await test("a token mail goes to the stored address, not the typed one", async () => {
    resetStore(); init();
    addUser(2, "ann@x.test", "first-password-1", true);
    await H.passwordResetRequest(mkReq({ email: "ann@X.TEST" }), mkRes());
    await H.magicLink(mkReq({ email: "ann@X.TEST" }), mkRes());
    authFlows.sendPasswordReset("ann@X.Test", "https://app.test");
    authFlows.sendMagicLink("ann@X.Test", "https://app.test");
    assertEq(sentTo("ann@x.test"), 4, "all four to the stored address");
    assertEq(sentTo("ann@X.TEST") + sentTo("ann@X.Test"), 0, "none to the typed");
});

await test("verify resend: lookalike mails nothing; the stored address gets it", async () => {
    resetStore(); init();
    addUser(3, "eve@x.test", "first-password-1", false);
    await H.verifyResend(mkReq({ email: "Éve@x.test" }), mkRes());
    await H.verifyResend(mkReq({ email: "\u0435ve@x.test" }), mkRes());
    assertEq(S.sent.length, 0, "nothing for a lookalike");
    await H.verifyResend(mkReq({ email: "EVE@X.test" }), mkRes());
    assertEq(sentTo("eve@x.test"), 1, "an ASCII case variant: to the stored address");
    assertEq(sentTo("EVE@X.test"), 0, "never to the typed spelling");
});

await test("standardUsers: ASCII case-insensitive, accents exact, legacy rows found", () => {
    resetStore(); init();
    db.exec("DROP TABLE IF EXISTS au12_users_js");
    db.exec("CREATE TABLE au12_users_js (id TEXT PRIMARY KEY, "
            + "email TEXT NOT NULL UNIQUE COLLATE NOCASE, password_hash TEXT, "
            + "email_verified INTEGER NOT NULL DEFAULT 0, "
            + "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)");
    const users = authFlows.standardUsers({ table: "au12_users_js" });
    const id = users.create("Alice@x.test", "h");
    const stored = db.query("SELECT email FROM au12_users_js WHERE id = ?", [id]);
    assertEq(stored[0].email, "alice@x.test", "stored ASCII-lowercased");
    assertEq(users.findByEmail("alice@x.test").id, id, "lowercase finds it");
    assertEq(users.findByEmail("ALICE@X.TEST").id, id, "ASCII case variant finds it");
    assertEq(users.findByEmail("alicé@x.test"), null, "accent variant does not");
    db.exec("INSERT INTO au12_users_js (id, email, password_hash, email_verified, "
            + "created_at, updated_at) VALUES ('legacy', 'Bob@X.test', 'h', 0, 0, 0)");
    const legacy = users.findByEmail("bob@x.test");
    assertEq(legacy && legacy.id, "legacy", "a row stored with ASCII capitals is still found");
    db.exec("DROP TABLE au12_users_js");
});

// ── M: the email-change notice ─────────────────────────────────────

await test("the notice to the old address is not rate limited", async () => {
    resetStore(); init({ emailRateLimit: { limit: 3, window: 900 } });
    addUser(10, "old10@x.test", "first-password-1", true);
    for (let i = 0; i < 3; i++) await H.magicLink(mkReq({ email: "old10@x.test" }), mkRes());
    S.sent = [];
    const res = await startChange(10, "new10@x.test");
    assertEq(res.body && res.body.ok, true);
    assertEq(sentTo("old10@x.test"), 1, "notice sent anyway");
    assertEq(sentTo("new10@x.test"), 1, "confirm mail sent");
});

await test("a change whose notice cannot be sent is not started", async () => {
    for (const mode of ["throw", "false", "reject"]) {
        resetStore(); init();
        const u = addUser(11, "old11@x.test", "first-password-1", true);
        S.failNotice = mode;
        let res = await startChange(11, "new11@x.test");
        assertEq(res.code, 503, mode + ": 503");
        assertEq(sentTo("new11@x.test"), 0, mode + ": no confirm mail");
        S.failNotice = null;
        res = await startChange(11, "new11@x.test");
        assertEq(res.body && res.body.ok, true, mode + ": no pending row left behind");
        res = mkRes();
        await H.emailChangeConfirm(mkReq({ token: tokenIn("new11@x.test") }), res);
        assertEq(u.email, "new11@x.test", mode + ": the retried change confirms");
    }
});

// ── M: the recovery lock persists ──────────────────────────────────

await test("the recovery lock does not expire; unlockRecovery clears it", async () => {
    resetStore(); init();
    addUser(20, "old20@x.test", "first-password-1", true);
    const rtok = await confirmedChange(20, "old20@x.test", "new20@x.test");
    addUser(21, "old20@x.test", "squatter-pw-1", true);
    // The undo writes its lock with an expiry already past.
    init({ emailChangeTtl: -100 });
    let res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.code, 409, "cannot restore");
    init();
    authFlows._test.gcExpired();
    assertEq(authFlows._test.recoveryLocked(20), true, "still locked after expiry + gc");
    S.sent = [];
    await H.passwordResetRequest(mkReq({ email: "new20@x.test" }), mkRes());
    assertEq(sentTo("new20@x.test"), 0, "no reset while locked");
    res = await startChange(20, "other20@x.test");
    assertEq(res.code, 409, "changes paused while locked");
    assertEq(authFlows.unlockRecovery(20), true, "cleared");
    assertEq(authFlows.unlockRecovery(20), false, "nothing left to clear");
    await H.passwordResetRequest(mkReq({ email: "new20@x.test" }), mkRes());
    assertEq(sentTo("new20@x.test"), 1, "reset after the operator unlocks");
});

await test("the lock row is written before totpDisable; sessions go first", async () => {
    resetStore(); init();
    addUser(22, "old22@x.test", "first-password-1", true);
    const rtok = await confirmedChange(22, "old22@x.test", "new22@x.test");
    addUser(23, "old22@x.test", "squatter-pw-1", true);
    await H.emailChangeRevoke(mkReq({ token: rtok }), mkRes());
    assertEq(S.lockedAtTotp, true, "locked when totpDisable ran");
    assertEq(S.order[0], "sessions");
    assertEq(S.order[1], "totp");
});

// ── M: a restore the database refuses falls back to the lock ───────

await test("an undo whose restore the database refuses locks recovery", async () => {
    resetStore(); init();
    const u = addUser(30, "old30@x.test", "first-password-1", true);
    const rtok = await confirmedChange(30, "old30@x.test", "new30@x.test");
    S.failSetEmail = "old30@x.test";
    const res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.code, 409, "nothing to restore to after all");
    assertEq(u.email, "new30@x.test");
    assertEq(authFlows._test.recoveryLocked(30), true, "recovery locked");
    assertEq(crypto.verifyPassword("first-password-1", u.password_hash), false,
             "password made unusable");
    assertEq(S.totpOff.length, 1, "second factor dropped");
});

// ── Lows ───────────────────────────────────────────────────────────

await test("a revoke whose pending change is confirmed meanwhile undoes it", async () => {
    resetStore(); init();
    const u = addUser(40, "old40@x.test", "first-password-1", true);
    let res = await startChange(40, "new40@x.test");
    assertEq(res.body && res.body.ok, true);
    const ctok = tokenIn("new40@x.test"), rtok = tokenIn("old40@x.test");
    // The confirm lands (another instance) between the revoke's read of the
    // pending row and its delete.
    S.onGet = () => { H.emailChangeConfirm(mkReq({ token: ctok }), mkRes()); };
    res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(u.email, "old40@x.test", "the confirmed change was undone");
    assertEq(res.body && res.body.restored, true);
});

await test("a confirm whose address switch fails leaves no pending row", async () => {
    resetStore(); init();
    const u = addUser(41, "old41@x.test", "first-password-1", true);
    await startChange(41, "new41@x.test");
    S.failSetEmail = "new41@x.test";
    let threw = false;
    try { await H.emailChangeConfirm(mkReq({ token: tokenIn("new41@x.test") }), mkRes()); }
    catch (_e) { threw = true; }
    assertEq(threw, true, "the failure propagates");
    S.failSetEmail = null;
    const res = await startChange(41, "again41@x.test");
    assertEq(res.body && res.body.ok, true, "row deleted, not put back to pending");
    assertEq(u.email, "old41@x.test");
});

await test("an unreadable email_verified is held for the undo, restored unverified", async () => {
    resetStore(); init();
    const u = addUser(42, "old42@x.test", "first-password-1", "1");
    const rtok = await confirmedChange(42, "old42@x.test", "new42@x.test");
    assertEq(authFlows.emailReserved("old42@x.test"), true, "held");
    const res = mkRes();
    await H.emailChangeRevoke(mkReq({ token: rtok }), res);
    assertEq(res.body && res.body.restored, true);
    assertEq(u.email, "old42@x.test");
    assertEq(u.email_verified, false, "restored as unverified");
});

await test("init refuses email_change_notify without onPasswordReset", () => {
    resetStore();
    let msg = "";
    try { init({ onPasswordReset: null }); } catch (e) { msg = String(e && e.message); }
    assertEq(msg.indexOf("onPasswordReset") >= 0, true);
});

await test("init refuses enableTotp without totpDisable", () => {
    resetStore();
    let msg = "";
    try {
        init({ enableTotp: true, totpDisable: null,
               userTotpEnrolled: () => false, totpVerify: () => false });
    } catch (e) { msg = String(e && e.message); }
    assertEq(msg.indexOf("totpDisable") >= 0, true);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
console.log("auth audit 12: " + pass + " passed, " + fail + " failed");
