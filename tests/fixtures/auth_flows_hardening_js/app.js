// Auth-flows hardening fixture (JS). Mirror of the Lua fixture.

import { app }       from "hull:app";
import { authFlows } from "hull:web:auth-flows";
import { session }   from "hull:web:middleware:session";
import { cookie }    from "hull:web:cookie";

// The e2e binds its HIBP mock to this fixed port so the fixture
// can hardcode the endpoint. Hull's env cap isn't wired during
// top-level code (env.get would throw).
const HIBP_MOCK_PORT = 39911;

app.manifest({
    name: "auth-flows-hardening-fixture-js",
    modules: [
        "hull/web/auth-flows@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/http-server@1",
        "hull/db@1",
        "hull/log@1",
        "hull/json@1",
        "hull/time@1",
    ],
    hosts: ["127.0.0.1"],
});

session.init();

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

const hibpMock = "http://127.0.0.1:" + HIBP_MOCK_PORT + "/range/";

authFlows.init({
    stateSecret: "fixture-state-secret-aaaaaaaaaaaa",
    emailSend: (to, subject, html, text) => {
        sentEmails.push({ to, subject, text: text || html });
    },
    templates: {
        welcome:             c => ({ subject: "Welcome", text: "verify: " + c.verify_url }),
        verify:              c => ({ subject: "Verify",  text: "verify: " + (c.verify_url || c.link || "?") }),
        magic_link:          c => ({ subject: "Sign in", text: "link: " + c.link }),
        password_reset:      c => ({ subject: "Reset",   text: "link: " + c.link }),
        email_change:        c => ({ subject: "Confirm email change", text: "confirm: " + c.link }),
        email_change_notify: c => ({
            subject: "Email change requested",
            text: "Someone requested changing your email to "
                + c.new_email + ". If this wasn't you, click: " + c.revoke_url,
        }),
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
    maxFailedLogins:     3,
    lockoutDuration:     2,
    checkPwnedPasswords: true,
    pwnedEndpoint:       hibpMock,
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

app.get("/_emails",        (_r, res) => res.json(sentEmails));
app.post("/_emails/clear", (_r, res) => { sentEmails = []; res.json({ ok: true }); });
app.get("/_me", (req, res) => {
    if (!req.ctx || !req.ctx.session) {
        return res.status(401).json({ error: "not signed in" });
    }
    res.json(req.ctx.session);
});
