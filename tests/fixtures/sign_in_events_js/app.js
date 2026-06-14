// Sign-in events e2e fixture (JS). Mirror of the Lua fixture.

import { app }       from "hull:app";
import { authFlows } from "hull:web:auth-flows";
import { auditLog }  from "hull:web:middleware:audit-log";
import { session }   from "hull:web:middleware:session";
import { cookie }    from "hull:web:cookie";

app.manifest({
    name: "sign-in-events-fixture-js",
    modules: [
        "hull/web/auth-flows@1",
        "hull/web/middleware/audit-log@1",
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
auditLog.init({
    retainDays: 365,
    fingerprintSalt: "sign-in-events-fixture-fingerprint-salt",
});

const usersByEmail = {};
const usersById    = {};
let nextId         = 0;
let sentEmails     = [];
let newDeviceAlerts = [];

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
    emailRateLimit: false,  // see auth_flows_js/app.js
    trustRequestHost: true, // see auth_flows_js/app.js (round-9 HIGH-1)
    emailSend: (to, subject, html, text) => {
        sentEmails.push({ to, subject, text: text || html });
    },
    templates: {
        welcome:        c => ({ subject: "Welcome", text: "verify: " + c.verify_url }),
        verify:         c => ({ subject: "Verify",  text: "verify: " + (c.verify_url || c.link || "?") }),
        magic_link:     c => ({ subject: "Sign in", text: "link: " + c.link }),
        password_reset: c => ({ subject: "Reset",   text: "link: " + c.link }),
        email_change:   c => ({ subject: "Confirm email change", text: "confirm: " + c.link }),
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
    // Canonical wiring: session.loginHandler owns the audit row and
    // the new-device hook. Same line works for OAuth via oauth.init's
    // onLogin. Cookie name is "session" here for fixture continuity
    // (the stdlib default is "hull_session").
    onLogin: session.loginHandler(cookie, {
        name: "session",
        cookieOpts: { path: "/", httpOnly: true, sameSite: "Lax" },
        auditLog: auditLog,
        onNewDevice: (_req, _res, user) => {
            newDeviceAlerts.push({ user_id: user.id, email: user.email });
        },
    }),
    onLogout: session.logoutHandler(cookie, { name: "session" }),
    signInLog: true,
    onPasswordReset: (_req, _res, user) => {
        session.destroyAll(user.id);
    },
});

authFlows.routes(app);

app.use("*", "/*", (req, _res) => {
    const cookies = cookie.parse(req.headers.cookie || "");
    if (cookies.session) {
        const data = session.load(cookies.session);
        if (data) {
            req.ctx = req.ctx || {};
            req.ctx.session    = data;
            req.ctx.session_id = cookies.session;
            req.ctx.user_id    = data.user_id;
        }
    }
    return 0;
});

app.get("/_emails",            (_r, res) => res.json(sentEmails));
app.post("/_emails/clear",     (_r, res) => { sentEmails = []; res.json({ ok: true }); });
app.get("/_new_devices",       (_r, res) => res.json(newDeviceAlerts));
app.post("/_new_devices/clear",(_r, res) => { newDeviceAlerts = []; res.json({ ok: true }); });
app.get("/_me", (req, res) => {
    if (!req.ctx || !req.ctx.session) {
        return res.status(401).json({ error: "not signed in" });
    }
    res.json(req.ctx.session);
});

app.get("/_devices", (req, res) => {
    if (!req.ctx || !req.ctx.user_id) return res.status(401).json({});
    res.json({
        sessions: session.listForUser(req.ctx.user_id),
        events:   auditLog.list(req.ctx.user_id, { limit: 50 }),
        devices:  auditLog.listDevices(req.ctx.user_id),
    });
});

app.post("/_revoke_others", (req, res) => {
    if (!req.ctx || !req.ctx.user_id) return res.status(401).json({});
    const n = session.destroyOthers(req.ctx.session_id, req.ctx.user_id);
    res.json({ ok: true, revoked: n });
});
