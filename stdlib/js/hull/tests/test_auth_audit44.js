// test_auth_audit44.js - auth stdlib regressions from audit 44 (JS).
//
// oauth: stateTtl / jwksTtl must be positive numbers; a failed JWKS fetch is
// throttled like a fetched one (JWKS_MIN_REFRESH) and fetched once per
// callback; a token reply that is not JSON answers 502. session: the login
// audit-metadata scrub reaches nested objects and matches camelCase /
// kebab-case keys; data that cannot be decoded destroys the row.
// auth-flows: the request Host is matched to trustedHosts without case;
// cleanup() reaps the lockout rows unknown addresses leave. rbac: a malformed
// session user id answers 401, and a role / permission argument is checked
// when the middleware is made. auth: `optional` must be the literal true.
// The Lua twin is test_auth_audit44.lua.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): auth-flows, session and rbac need the db.

import { crypto } from "hull:crypto";
import { encoding } from "hull:encoding";
import { cookie } from "hull:web:cookie";
import { oauth } from "hull:web:middleware:oauth";
import { session } from "hull:web:middleware:session";
import { rbac } from "hull:web:middleware:rbac";
import { auth } from "hull:web:middleware:auth";
import { authFlows } from "hull:web:auth-flows";

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
    try { fn(); return null; } catch (e) { return String(e && e.message); }
}

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

function mkReq(headers, ctx) {
    return { method: "POST", path: "/x", headers: headers || { host: "app.test" },
             ctx: ctx || {}, remote_addr: "192.0.2.7", query: {}, params: {},
             header: () => null };
}

// ── oauth ──────────────────────────────────────────────────────────

const OT = __testHooks("hull:web:middleware:oauth");

function oauthInit(extra) {
    OT.reset();
    oauth.init(Object.assign({
        secret: "s".repeat(32), baseUrl: "https://app.test",
        providers: { g: { preset: "google", clientId: "cid" } },
    }, extra || {}));
}

function callbackHandler() {
    let got = null;
    oauth.routes({
        get: (path, fn) => { if (path.includes("callback")) got = fn; },
        post: () => {},
    });
    return got;
}

function callbackReq() {
    const v = OT.signState({ provider: "g", verifier: "v", state: "st", nonce: "n" });
    return { method: "GET", path: "/auth/g/callback",
             headers: { host: "app.test", cookie: "_oauth_state=" + v },
             params: { provider: "g" }, query: { code: "c", state: "st" },
             ctx: {}, remote_addr: "192.0.2.7" };
}

const b64u = (s) => encoding.base64.encode(s, { url: true });
const ID_TOKEN = b64u('{"alg":"RS256","kid":"k1"}') + "." + b64u("{}") + "." + b64u("sig");

await test("oauth_audit44.init_ttl_types", () => {
    for (const k of ["stateTtl", "jwksTtl"]) {
        for (const bad of ["600", -1, NaN, Infinity, {}, true]) {
            const err = threw(() => oauthInit({ [k]: bad }));
            assertEq(typeof err === "string" && err.includes(k), true,
                     k + "=" + String(bad) + ": " + err);
        }
        // 0 is refused too (it used to fall back to the default silently).
        assertEq(typeof threw(() => oauthInit({ [k]: 0 })), "string", k + "=0");
        assertEq(threw(() => oauthInit({ [k]: 300 })), null, k + " = 300 taken");
    }
    oauthInit({ stateTtl: 120 });
    const r = OT.verifyState(OT.signState({ provider: "g" }));
    assertEq(r[0] !== null, true, "a state signed under a numeric ttl verifies: " + r[1]);
    OT.reset();
});

await test("oauth_audit44.jwks_failure_throttled", async () => {
    oauthInit();
    let gets = 0;
    OT.setGet(async () => { gets++; return { status: 503, body: "down" }; });
    OT.setPost(async () => ({ status: 200, body: JSON.stringify({ id_token: ID_TOKEN }) }));
    try {
        const h = callbackHandler();
        for (let i = 0; i < 3; i++) {
            const res = mkRes();
            await h(callbackReq(), res);
            assertEq(res.code, 400, "auth failed");
        }
    } finally {
        OT.setGet(null); OT.setPost(null); OT.reset();
    }
    assertEq(gets, 1, "one JWKS fetch for three callbacks inside a minute");
});

await test("oauth_audit44.token_endpoint_non_json_is_502", async () => {
    oauthInit();
    OT.setPost(async () => ({ status: 200, body: "<html>not json</html>" }));
    const res = mkRes();
    try { await callbackHandler()(callbackReq(), res); }
    finally { OT.setPost(null); OT.reset(); }
    assertEq(res.code, 502, "bad gateway");
});

// ── session ────────────────────────────────────────────────────────

await test("session_audit44.scrub_nested_and_camel", async () => {
    session.init({ cleanup: false });
    let recorded;
    const h = session.loginHandler(cookie, {
        auditLog: { record: (_a, _b, _c, o) => { recorded = o.metadata; } },
        auditMetadata: () => ({
            factors: "oauth:g",
            idToken: "it", accessToken: "at", "refresh-token": "rt",
            PasswordHash: "ph",
            oauth: { provider: "g", tokens: { access_token: "at" },
                     claims: { sub: "x" }, nested: { idToken: "it", keep: 2 } },
            list: [{ Secret: "s", keep: 1 }],
        }),
    });
    await h(mkReq(), mkRes(), { id: 44 });
    assertEq(typeof recorded, "object", "recorded");
    assertEq(recorded.factors, "oauth:g", "kept");
    assertEq(recorded.idToken, undefined, "idToken");
    assertEq(recorded.accessToken, undefined, "accessToken");
    assertEq(recorded["refresh-token"], undefined, "refresh-token");
    assertEq(recorded.PasswordHash, undefined, "PasswordHash");
    assertEq(typeof recorded.oauth, "object", "nested object kept");
    assertEq(recorded.oauth.provider, "g", "nested value kept");
    assertEq(recorded.oauth.tokens, undefined, "nested tokens");
    assertEq(recorded.oauth.claims, undefined, "nested claims");
    assertEq(recorded.oauth.nested.idToken, undefined, "two levels down");
    assertEq(recorded.oauth.nested.keep, 2, "two levels down kept");
    assertEq(Array.isArray(recorded.list), true, "an array stays an array");
    assertEq(recorded.list[0].Secret, undefined, "inside an array");
    assertEq(recorded.list[0].keep, 1, "inside an array kept");
});

await test("session_audit44.scrub_is_depth_bounded", async () => {
    session.init({ cleanup: false });
    let recorded;
    const cyc = {};
    cyc.self = cyc;
    const h = session.loginHandler(cookie, {
        auditLog: { record: (_a, _b, _c, o) => { recorded = o.metadata; } },
        auditMetadata: () => ({ factors: "pw", loop: cyc }),
    });
    await h(mkReq(), mkRes(), { id: 45 });
    assertEq(recorded.factors, "pw", "recorded");
});

await test("session_audit44.undecodable_data_destroys_row", () => {
    session.init({ cleanup: false });
    // Data nested past the JSON decoder's depth ceiling: it encodes, but the
    // stored text cannot be read back.
    const deep = {};
    let cur = deep;
    for (let i = 0; i < 300; i++) { cur.n = {}; cur = cur.n; }
    const sid = session.create({ user_id: 1, deep });
    assertEq(session.load(sid), null, "no session");
    assertEq(session.update(sid, { user_id: 1 }), false, "the row is gone");
});

// ── auth ───────────────────────────────────────────────────────────

await test("auth_audit44.optional_requires_literal_true", () => {
    session.init({ cleanup: false });
    for (const v of ["no", 1, "false", {}]) {
        let res = mkRes();
        assertEq(auth.sessionMiddleware({ optional: v })(mkReq(), res), 1, "session " + v);
        assertEq(res.code, 401, "session 401");
        res = mkRes();
        assertEq(auth.jwtMiddleware({ secret: "j".repeat(32), optional: v })(mkReq(), res),
                 1, "jwt " + v);
        assertEq(res.code, 401, "jwt 401");
    }
    assertEq(auth.sessionMiddleware({ optional: true })(mkReq(), mkRes()), 0, "true");
    assertEq(auth.jwtMiddleware({ secret: "j".repeat(32), optional: true })(mkReq(), mkRes()),
             0, "jwt true");
});

// ── rbac ───────────────────────────────────────────────────────────

await test("rbac_audit44.require_role_bad_uid", () => {
    rbac.init();
    for (const bad of [null, undefined, false, 5, "", {}, [], ["admin", 5], [""]]) {
        assertEq(typeof threw(() => rbac.requireRole(bad)), "string", "role " + String(bad));
        assertEq(typeof threw(() => rbac.requirePermission(bad)), "string", "perm " + String(bad));
    }
    assertEq(threw(() => rbac.requireRole("admin")), null, "a name");
    assertEq(threw(() => rbac.requireRole(["admin", "editor"])), null, "a list");
});

await test("rbac_audit44.empty_user_id_is_401", () => {
    rbac.init();
    rbac.defineRole("a44admin", ["a44.read"]);
    rbac.assign("44", "a44admin");
    const mws = [rbac.requireRole("a44admin"), rbac.requirePermission("a44.read")];
    for (const uid of [1.5, {}, true, "", -0.5]) {
        mws.forEach((mw, i) => {
            const res = mkRes();
            let rv;
            const err = threw(() => { rv = mw(mkReq(null, { session: { user_id: uid } }), res); });
            assertEq(err, null, "mw " + i + " uid " + String(uid));
            assertEq(rv, 1, "stopped");
            assertEq(res.code, 401, "mw " + i + " uid " + String(uid));
        });
    }
    const gmw = rbac.requireRole("a44admin", { getUserId: () => 2.5 });
    const res = mkRes();
    assertEq(gmw(mkReq(), res), 1, "getUserId");
    assertEq(res.code, 401, "getUserId 401");
    mws.forEach((mw, i) => {
        assertEq(mw(mkReq(null, { session: { user_id: 44 } }), mkRes()), 0, "mw " + i + " holder");
    });
});

// ── auth-flows ─────────────────────────────────────────────────────

const AT = __testHooks("hull:web:auth-flows");
let sent = [];
const users = new Map();

function afInit(extra) {
    AT.reset();
    sent = [];
    authFlows.init(Object.assign({
        secret: "s".repeat(32),
        trustedHosts: ["app.example.com"],
        emailRateLimit: false,
        emailSend: (to, subject, _html, text) => { sent.push({ to, subject, text }); },
        templates: {
            welcome:        (c) => ({ subject: "w", text: c.verify_url }),
            magic_link:     (c) => ({ subject: "m", text: c.link }),
            password_reset: (c) => ({ subject: "p", text: c.link }),
            email_change:   (c) => ({ subject: "e", text: c.link }),
            email_change_notify: (c) => ({ subject: "n", text: c.revoke_url }),
        },
        userFindByEmail: (e) => users.get(e) || null,
        userGet: (id) => {
            for (const u of users.values()) if (String(u.id) === String(id)) return u;
            return null;
        },
        userCreate: () => null,
        userSetPassword: () => {},
        userSetEmail: () => {},
        userSetEmailVerified: () => {},
        onLogin: (_req, res) => { res.json({ ok: true }); },
        onPasswordReset: () => {},
    }, extra || {}));
}

await test("authflows_audit44.origin_host_case", async () => {
    users.set("ann@x.test", { id: 7, email: "ann@x.test", email_verified: true,
                              password_hash: crypto.hashPassword("pw-ann-1234567") });
    afInit();
    for (const host of ["App.Example.com", "APP.EXAMPLE.COM", "app.example.com"]) {
        assertEq(AT.originFor({ headers: { host } }), "https://app.example.com", host);
    }
    assertEq(AT.originFor({ headers: { host: "evil.example.com" } }), null, "other host");
    // The cross-site check and the link agree: the mail goes out.
    const req = mkReq({ host: "App.Example.com", "content-type": "application/json",
                        "sec-fetch-site": "same-origin" });
    req.body = JSON.stringify({ email: "ann@x.test" });
    await AT.handlers.passwordResetRequest(req, mkRes());
    await null;
    assertEq(sent.length, 1, "reset mail sent");
    assertEq(sent[0].text.startsWith("https://app.example.com/"), true, String(sent[0].text));
    users.delete("ann@x.test");
});

await test("auth_flows_audit44.unknown_login_rows_reaped_by_cleanup", () => {
    afInit({ cleanup: false });
    assertEq(typeof authFlows.cleanup, "function", "authFlows exports cleanup()");
    const old = AT.unknownLoginKey("ghost-old@x.test");
    const fresh = AT.unknownLoginKey("ghost-new@x.test");
    AT.bumpFailedLogin(old, 5);
    AT.bumpFailedLogin(fresh, 5);
    AT.backdateLoginAttempt(old, 2 * 86400);
    assertEq(AT.loginFailedCount(old), 1, "old row there");
    authFlows.cleanup();
    assertEq(AT.loginFailedCount(old), 0, "an old unknown-address row is reaped");
    assertEq(AT.loginFailedCount(fresh), 1, "a recent one is kept");
    assertEq(typeof threw(() => afInit({ cleanupAt: 5 })), "string", "cleanupAt must be a string");
});

console.log("auth audit 44: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
