// Auth-flows + TOTP 2FA composition fixture (JS). Mirror of
// tests/fixtures/auth_flows_2fa_lua/app.lua.

import { app }        from "hull:app";
import { authFlows }  from "hull:web:auth-flows";
import { totp }       from "hull:web:middleware:totp";
import { session }    from "hull:web:middleware:session";
import { cookie }     from "hull:web:cookie";
import { json }       from "hull:json";

app.manifest({
    name: "auth-flows-2fa-fixture-js",
    modules: [
        "hull/web/auth-flows@1",
        "hull/web/middleware/totp@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/http-server@1",
        "hull/db@1",
        "hull/log@1",
        "hull/json@1",
        "hull/time@1",
    ],
});

session.init();
totp.init({ issuer: "auth-flows-2fa-test" });

const usersByEmail = {};
const usersById    = {};
let nextId         = 0;
let sentEmails     = [];

function userCreate(email, pwhash) {
    nextId += 1;
    const id = "u" + nextId;
    const u = { id, email, password_hash: pwhash, email_verified: false };
    usersByEmail[email] = u;
    usersById[id] = u;
    return id;
}

authFlows.init({
    stateSecret: "fixture-state-secret-aaaaaaaaaaaa",
    emailSend: (to, subject, html, text) => {
        sentEmails.push({ to, subject, text: text || html });
    },
    templates: {
        welcome:        c => ({ subject: "Welcome", text: "verify: " + c.verify_url }),
        verify:         c => ({ subject: "Verify",  text: "verify: " + (c.verify_url || c.link || "?") }),
        magic_link:     c => ({ subject: "Sign in", text: "link: " + c.link }),
        password_reset: c => ({ subject: "Reset",   text: "link: " + c.link }),
        email_change:   c => ({ subject: "Confirm email change", text: "link: " + c.link }),
    },
    userFindByEmail: email => usersByEmail[email],
    userGet:         id    => usersById[id],
    userCreate,
    userSetPassword: (id, pwhash) => { usersById[id].password_hash = pwhash; },
    userSetEmail:    (id, email) => {
        const u = usersById[id];
        delete usersByEmail[u.email];
        u.email = email;
        usersByEmail[email] = u;
    },
    userSetEmailVerified: (id, v) => { usersById[id].email_verified = v; },
    enableTotp:       true,
    userTotpEnrolled: userId => totp.enrolled(userId),
    // totp.verify returns [ok, kind] as a tuple — an array is
    // truthy in JS, so we MUST unwrap it before handing to
    // auth-flows (which does `if (!ok) ...`).
    totpVerify:       (user, code) => totp.verify(user.id || user.user_id, code)[0],
    onLogin: (req, res, user) => {
        const sid = session.create({ user_id: user.id, email: user.email });
        res.header("Set-Cookie", cookie.serialize("session", sid,
            { path: "/", httpOnly: true, sameSite: "Lax" }));
        res.json({ ok: true, user_id: user.id, email: user.email });
    },
    onLogout: (req, res) => {
        const cookies = cookie.parse(req.headers.cookie || "");
        if (cookies.session) session.destroy(cookies.session);
        res.header("Set-Cookie", cookie.clear("session", { path: "/" }));
        res.json({ ok: true });
    },
});

authFlows.routes(app);

app.use("*", "/*", (req, _res) => {
    const cookies = cookie.parse(req.headers.cookie || "");
    if (cookies.session) {
        const data = session.load(cookies.session);
        if (data) {
            req.ctx = req.ctx || {};
            req.ctx.session = data;
            req.ctx.user_id = data.user_id;
        }
    }
    return 0;
});

app.post("/_totp_enroll", (req, res) => {
    const b = json.decode(req.body || "") || {};
    const u = usersByEmail[b.email];
    if (!u) return res.status(404).json({ error: "no user" });
    const r = totp.enroll(u.id);
    res.json({
        secret: r.secretBase32,
        otpauth_url: r.otpauthUrl,
        recovery_codes: r.recoveryCodes,
    });
});

app.post("/_totp_confirm", (req, res) => {
    const b = json.decode(req.body || "") || {};
    const u = usersByEmail[b.email];
    if (!u) return res.status(404).json({ error: "no user" });
    const ok = totp.confirm(u.id, b.code);
    if (!ok) return res.status(400).json({ error: "no" });
    res.json({ ok: true });
});

app.get("/_emails",        (_r, res) => res.json(sentEmails));
app.post("/_emails/clear", (_r, res) => { sentEmails = []; res.json({ ok: true }); });
app.get("/_me", (req, res) => {
    if (!req.ctx || !req.ctx.session) {
        return res.status(401).json({ error: "not signed in" });
    }
    res.json(req.ctx.session);
});
