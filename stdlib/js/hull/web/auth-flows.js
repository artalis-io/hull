/**
 * @file hull:web:auth-flows
 * @module hull:web:auth-flows
 * @description Transactional auth flows: registration / email-verify /
 *              login / password-reset / magic-link / email-change.
 *
 * Lua parity: same surface as `hull.web.auth-flows`. snake_case
 * option keys ↔ camelCase here; routes + token format on the wire
 * are byte-identical so a Lua-Hull → JS-Hull migration of the same
 * SQLite DB works without re-issuing pending tokens.
 *
 * @license AGPL-3.0-or-later
 *
 * See the Lua module header for the full security model, threat-
 * model notes, and the rationale for each option default.
 */

import { crypto } from "hull:crypto";
import { db }     from "hull:db";
import { time }   from "hull:time";
import { json }   from "hull:json";

const _state = {
    stateSecretHex:      null,
    verifyTtl:           86400,
    resetTtl:            3600,
    magicLinkTtl:        600,
    emailChangeTtl:      86400,
    prefix:              "/auth",
    enumerationSafe:     true,
    magicLinkAutoSignup: false,
    requireVerifiedEmail: true,
    emailSend:           null,
    templates:           {},
    userFindByEmail:        null,
    userGet:                null,
    userCreate:             null,
    userSetPassword:        null,
    userSetEmail:           null,
    userSetEmailVerified:   null,
    onLogin:                null,
    onLogout:               null,
    // TOTP composition. Off by default; opt in by setting
    // enableTotp = true plus userTotpEnrolled + totpVerify. See
    // the Lua module header for the security model.
    enableTotp:             false,
    userTotpEnrolled:       null,
    totpVerify:             null,
    totpPendingTtl:         300,
    totpPendingRedirect:    null,
    verifyRedirect:      "/",
    loginRedirect:       "/",
    initialized:         false,
};

const SCHEMA = `
CREATE TABLE IF NOT EXISTS _hull_auth_used_tokens (
    token_hash  TEXT PRIMARY KEY,
    used_at     INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS _hull_auth_pending_email_changes (
    user_id     TEXT PRIMARY KEY,
    new_email   TEXT NOT NULL,
    token_hash  TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS _hull_auth_used_tokens_exp
    ON _hull_auth_used_tokens(expires_at);
CREATE INDEX IF NOT EXISTS _hull_auth_pending_email_changes_exp
    ON _hull_auth_pending_email_changes(expires_at);
`;

const ACTIONS = {
    verify_email:   "verify",
    password_reset: "reset",
    magic_link:     "magic",
    email_change:   "email_change",
    // Pending 2FA token. Multi-use within TTL (allows retry on
    // typo); burned on a successful code verify.
    totp_pending:   "totp_pending",
};

function bytesToHex(s) {
    let h = "";
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i) & 0xff;
        h += (c < 16 ? "0" : "") + c.toString(16);
    }
    return h;
}

function issueToken(userId, action, ttl, extra) {
    const payload = {
        sub:    userId,
        action: action,
        exp:    time.now() + ttl,
        nonce:  crypto.base64urlEncode(crypto.random(16)),
    };
    if (extra) {
        for (const k in extra) {
            if (Object.prototype.hasOwnProperty.call(extra, k)) {
                payload[k] = extra[k];
            }
        }
    }
    const body = crypto.base64urlEncode(json.encode(payload));
    const tag = crypto.hmacSha256(body, _state.stateSecretHex);
    return body + "." + tag;
}

// Verify signature + action + expiry WITHOUT marking the token
// used. Mirrors Lua's parse_token — the TOTP flow uses it
// directly so the pending token stays usable across retry-on-
// typo attempts and is burned only on successful code verify.
function parseToken(token, expectedAction) {
    if (typeof token !== "string" || token === "") return [null, "missing"];
    const dot = token.indexOf(".");
    if (dot < 0) return [null, "malformed"];
    const body = token.substring(0, dot);
    const tag  = token.substring(dot + 1);

    // crypto.hmacSha256Verify throws on malformed-hex input — catch
    // so a user-typed junk token returns a clean "bad tag".
    let valid = false;
    try {
        valid = crypto.hmacSha256Verify(body, _state.stateSecretHex, tag);
    } catch (_e) {
        return [null, "bad tag"];
    }
    if (!valid) return [null, "bad tag"];

    const raw = crypto.base64urlDecode(body);
    if (raw === null || raw === undefined) return [null, "bad encoding"];
    let env;
    try { env = json.decode(raw); } catch (_e) { return [null, "bad json"]; }
    if (!env || typeof env !== "object") return [null, "bad json"];
    if (env.action !== expectedAction) return [null, "wrong action"];
    if (typeof env.exp !== "number" || time.now() >= env.exp) {
        return [null, "expired"];
    }
    return [env, null];
}

function markTokenUsed(token, exp) {
    const tokenHash = crypto.sha256(token);
    const rc = db.exec(
        "INSERT OR IGNORE INTO _hull_auth_used_tokens "
        + "(token_hash, used_at, expires_at) VALUES (?, ?, ?)",
        [tokenHash, time.now(), exp]);
    return rc > 0;
}

function tokenAlreadyUsed(token) {
    const tokenHash = crypto.sha256(token);
    const rows = db.query(
        "SELECT 1 FROM _hull_auth_used_tokens WHERE token_hash = ? LIMIT 1",
        [tokenHash]);
    return rows !== null && rows !== undefined && rows.length > 0;
}

// Atomic verify + mark-used. Used by every flow except TOTP-
// pending; two concurrent click-throughs of the same link must
// not both succeed.
function consumeToken(token, expectedAction) {
    const r = parseToken(token, expectedAction);
    if (r[1]) return [null, r[1]];
    const env = r[0];
    if (!markTokenUsed(token, env.exp)) return [null, "replayed"];
    return [env, null];
}

function renderTemplate(name, ctx) {
    const tpl = _state.templates[name];
    if (typeof tpl !== "function") {
        throw new Error("auth-flows: template '" + name + "' not provided in init.templates");
    }
    const r = tpl(ctx);
    if (!r || typeof r.subject !== "string"
        || (typeof r.html !== "string" && typeof r.text !== "string")) {
        throw new Error("auth-flows: template '" + name
            + "' must return { subject, html?, text? }");
    }
    return r;
}

function sendEmail(to, templateName, ctx) {
    const r = renderTemplate(templateName, ctx);
    _state.emailSend(to, r.subject, r.html, r.text);
}

function gcExpired() {
    const now = time.now();
    db.exec("DELETE FROM _hull_auth_used_tokens WHERE expires_at < ?", [now]);
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE expires_at < ?",
            [now]);
}

function parseBody(req) {
    const body = req.body || "";
    if (body.length === 0) return {};
    const ct = (req.headers && req.headers["content-type"]) || "";
    if (ct.indexOf("application/json") >= 0) {
        try { const t = json.decode(body); return (t && typeof t === "object") ? t : {}; }
        catch (_e) { return {}; }
    }
    const out = {};
    const pairs = body.split("&");
    for (let i = 0; i < pairs.length; i++) {
        const eq = pairs[i].indexOf("=");
        if (eq >= 0) {
            const k = pairs[i].substring(0, eq);
            let v = pairs[i].substring(eq + 1).replace(/\+/g, " ");
            try { v = decodeURIComponent(v); } catch (_e) { /* keep raw */ }
            out[k] = v;
        }
    }
    return out;
}

function isEmailIsh(s) {
    if (typeof s !== "string") return false;
    if (s.length < 3 || s.length > 254) return false;
    const at = s.indexOf("@");
    if (at < 1 || at === s.length - 1) return false;
    const dot = s.indexOf(".", at);
    if (dot < 0 || dot === at + 1 || dot === s.length - 1) return false;
    return true;
}

function genericOk(res) { res.json({ ok: true }); }

function userId(user) { return user.id || user.user_id; }

function originFor(req) {
    const proto = (req.headers && req.headers["x-forwarded-proto"]) || "http";
    const host  = (req.headers && req.headers["x-forwarded-host"])
                  || (req.headers && req.headers.host)
                  || "localhost";
    return proto + "://" + host;
}

// ── Route handlers ─────────────────────────────────────────────────

function handleRegister(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    if (typeof body.password !== "string" || body.password.length < 8) {
        return res.status(400).json({ error: "password too short" });
    }
    const existing = _state.userFindByEmail(body.email);
    if (existing) return genericOk(res);

    const pwHash = crypto.hashPassword(body.password);
    const uid = _state.userCreate(body.email, pwHash);
    const user = _state.userGet(uid);
    if (!user) {
        return res.status(500).json({
            error: "user_create returned an id that user_get cannot resolve" });
    }

    const token = issueToken(uid, ACTIONS.verify_email, _state.verifyTtl);
    const verifyUrl = originFor(req) + _state.prefix + "/verify?token=" + token;
    sendEmail(body.email, "welcome", { user, verify_url: verifyUrl, token });
    res.json({ ok: true });
}

function handleVerify(req, res) {
    const token = req.query && req.query.token;
    const result = consumeToken(token, ACTIONS.verify_email);
    if (!result[0]) {
        return res.status(400).html("verification failed: " + (result[1] || "?"));
    }
    _state.userSetEmailVerified(result[0].sub, true);
    gcExpired();
    res.redirect(_state.verifyRedirect);
}

// Minimal HTML form rendered on a magic-link click when 2FA is
// required and the app hasn't configured `totpPendingRedirect`.
// Only the pending token is interpolated; its alphabet is fixed
// (base64url body + hex tag) so no escaping needed.
function defaultTotpFormHtml(token) {
    return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
         + '<title>Two-factor verification</title></head>'
         + '<body style="font-family:sans-serif;max-width:360px;'
         + 'margin:4em auto;"><h1>Two-factor verification</h1>'
         + '<form method="POST" action="' + _state.prefix + '/totp-verify">'
         + '<input type="hidden" name="token" value="' + token + '">'
         + '<p><label>Code: <input name="code" autofocus '
         + 'autocomplete="one-time-code" inputmode="numeric" '
         + 'pattern="[0-9A-Za-z-]+"></label></p>'
         + '<button type="submit">Verify</button></form>'
         + '<p style="color:#666;font-size:smaller">Lost your device? '
         + 'Enter a recovery code instead.</p></body></html>';
}

function startTotpPending(req, res, user) {
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.totp_pending,
                              _state.totpPendingTtl);
    if (req.method === "POST") {
        return res.json({
            ok: true, pending_2fa: true, totp_token: token,
        });
    }
    if (_state.totpPendingRedirect) {
        const sep = _state.totpPendingRedirect.indexOf("?") >= 0 ? "&" : "?";
        return res.redirect(_state.totpPendingRedirect
                             + sep + "token=" + token);
    }
    res.html(defaultTotpFormHtml(token));
}

function handleLogin(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email) || typeof body.password !== "string") {
        return res.status(400).json({ error: "invalid credentials" });
    }
    const user = _state.userFindByEmail(body.email);
    if (!user || !user.password_hash
        || !crypto.verifyPassword(body.password, user.password_hash)) {
        return res.status(401).json({ error: "invalid credentials" });
    }
    if (_state.requireVerifiedEmail && !user.email_verified) {
        return res.status(403).json({ error: "email not verified" });
    }
    if (_state.enableTotp && _state.userTotpEnrolled(userId(user))) {
        return startTotpPending(req, res, user);
    }
    _state.onLogin(req, res, user);
}

function handleLogout(req, res) {
    if (_state.onLogout) _state.onLogout(req, res);
    else res.redirect("/");
}

function handleMagicLink(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    let user = _state.userFindByEmail(body.email);
    if (!user) {
        if (!_state.magicLinkAutoSignup) return genericOk(res);
        const uid = _state.userCreate(body.email, null);
        user = _state.userGet(uid);
    }
    const token = issueToken(userId(user), ACTIONS.magic_link,
        _state.magicLinkTtl);
    const link = originFor(req) + _state.prefix + "/magic-link/consume?token=" + token;
    sendEmail(body.email, "magic_link", { user, link, token });
    res.json({ ok: true });
}

function handleMagicLinkConsume(req, res) {
    const token = req.query && req.query.token;
    const result = consumeToken(token, ACTIONS.magic_link);
    if (!result[0]) {
        return res.status(400).html("magic link failed: " + (result[1] || "?"));
    }
    const user = _state.userGet(result[0].sub);
    if (!user) return res.status(400).html("magic link failed");
    if (!user.email_verified) {
        _state.userSetEmailVerified(userId(user), true);
        user.email_verified = true;
    }
    gcExpired();
    if (_state.enableTotp && _state.userTotpEnrolled(userId(user))) {
        return startTotpPending(req, res, user);
    }
    _state.onLogin(req, res, user);
}

// POST /auth/totp-verify { token, code } — second factor.
// Apps SHOULD rate-limit this route (e.g. ratelimit.middleware
// keyed on the body's token field) to bound brute-force on the
// 6-digit code space. The pending token is multi-use within TTL
// (lets users retry typos) and only burned on a successful code
// verify.
function handleTotpVerify(req, res) {
    if (!_state.enableTotp) {
        return res.status(404).json({ error: "totp not enabled" });
    }
    const body = parseBody(req);
    if (typeof body.token !== "string" || typeof body.code !== "string") {
        return res.status(400).json({ error: "missing token or code" });
    }
    const r = parseToken(body.token, ACTIONS.totp_pending);
    if (!r[0]) {
        return res.status(400).json({
            error: "totp failed: " + (r[1] || "?") });
    }
    if (tokenAlreadyUsed(body.token)) {
        return res.status(400).json({ error: "totp token already used" });
    }
    const env = r[0];
    const user = _state.userGet(env.sub);
    if (!user) return res.status(400).json({ error: "totp failed" });
    const ok = _state.totpVerify(user, body.code);
    if (!ok) return res.status(401).json({ error: "invalid code" });
    markTokenUsed(body.token, env.exp);
    gcExpired();
    _state.onLogin(req, res, user);
}

function handlePasswordResetRequest(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    const user = _state.userFindByEmail(body.email);
    if (!user) return genericOk(res);
    const token = issueToken(userId(user), ACTIONS.password_reset,
        _state.resetTtl);
    const link = originFor(req) + _state.prefix
        + "/password-reset/confirm?token=" + token;
    sendEmail(body.email, "password_reset", { user, link, token });
    res.json({ ok: true });
}

function handlePasswordResetConfirm(req, res) {
    const body = parseBody(req);
    if (typeof body.password !== "string" || body.password.length < 8) {
        return res.status(400).json({ error: "password too short" });
    }
    const result = consumeToken(body.token, ACTIONS.password_reset);
    if (!result[0]) {
        return res.status(400).json({
            error: "reset failed: " + (result[1] || "?") });
    }
    const user = _state.userGet(result[0].sub);
    if (!user) return res.status(400).json({ error: "reset failed" });
    _state.userSetPassword(result[0].sub, crypto.hashPassword(body.password));
    gcExpired();
    res.json({ ok: true });
}

function handleEmailChange(req, res) {
    const uid = req.ctx && req.ctx.user_id;
    if (!uid) return res.status(401).json({ error: "not authenticated" });
    const body = parseBody(req);
    if (!isEmailIsh(body.new_email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    if (_state.userFindByEmail(body.new_email)) {
        return res.status(409).json({ error: "email already in use" });
    }

    const now = time.now();
    const token = issueToken(uid, ACTIONS.email_change,
        _state.emailChangeTtl, { new_email: body.new_email });
    const tokenHash = crypto.sha256(token);
    db.exec(
        "INSERT OR REPLACE INTO _hull_auth_pending_email_changes "
        + "(user_id, new_email, token_hash, created_at, expires_at) "
        + "VALUES (?, ?, ?, ?, ?)",
        [uid, body.new_email, tokenHash, now, now + _state.emailChangeTtl]);

    const user = _state.userGet(uid);
    const link = originFor(req) + _state.prefix
        + "/email-change/confirm?token=" + token;
    sendEmail(body.new_email, "email_change", {
        user, link, token, new_email: body.new_email,
    });
    res.json({ ok: true });
}

function handleEmailChangeConfirm(req, res) {
    const token = req.query && req.query.token;
    const result = consumeToken(token, ACTIONS.email_change);
    if (!result[0]) {
        return res.status(400).html("email change failed: " + (result[1] || "?"));
    }
    const env = result[0];
    const user = _state.userGet(env.sub);
    if (!user) return res.status(400).html("email change failed");
    const rows = db.query(
        "SELECT new_email FROM _hull_auth_pending_email_changes "
        + "WHERE user_id = ?", [env.sub]);
    if (!rows || rows.length === 0 || rows[0].new_email !== env.new_email) {
        return res.status(400).html("email change failed");
    }
    _state.userSetEmail(env.sub, env.new_email);
    _state.userSetEmailVerified(env.sub, true);
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            [env.sub]);
    gcExpired();
    res.redirect(_state.verifyRedirect);
}

function registerRoutes(app) {
    const p = _state.prefix;
    app.post(p + "/register",                 handleRegister);
    app.get (p + "/verify",                   handleVerify);
    app.post(p + "/login",                    handleLogin);
    app.post(p + "/logout",                   handleLogout);
    app.post(p + "/magic-link",               handleMagicLink);
    app.get (p + "/magic-link/consume",       handleMagicLinkConsume);
    app.post(p + "/password-reset/request",   handlePasswordResetRequest);
    app.post(p + "/password-reset/confirm",   handlePasswordResetConfirm);
    app.post(p + "/email-change",             handleEmailChange);
    app.get (p + "/email-change/confirm",     handleEmailChangeConfirm);
    // Registered unconditionally; the handler returns 404 when
    // enableTotp is false (clearer than a route-level 404).
    app.post(p + "/totp-verify",              handleTotpVerify);
}

// ── Public API ─────────────────────────────────────────────────────

function init(opts) {
    opts = opts || {};
    if (typeof opts.stateSecret !== "string" || opts.stateSecret.length < 32) {
        throw new Error("auth-flows.init: stateSecret must be a string >= 32 bytes");
    }
    if (typeof opts.emailSend !== "function") {
        throw new Error("auth-flows.init: emailSend(to, subject, html, text) required");
    }
    if (!opts.templates || typeof opts.templates !== "object") {
        throw new Error("auth-flows.init: templates object required");
    }
    const requiredUser = [
        "userFindByEmail", "userGet", "userCreate",
        "userSetPassword", "userSetEmail", "userSetEmailVerified",
    ];
    const missing = [];
    for (let i = 0; i < requiredUser.length; i++) {
        if (typeof opts[requiredUser[i]] !== "function") {
            missing.push(requiredUser[i]);
        }
    }
    if (missing.length > 0) {
        throw new Error("auth-flows.init: missing required callbacks: "
                        + missing.join(", "));
    }
    if (typeof opts.onLogin !== "function") {
        throw new Error("auth-flows.init: onLogin(req, res, user) required");
    }
    if (opts.enableTotp) {
        if (typeof opts.userTotpEnrolled !== "function") {
            throw new Error("auth-flows.init: userTotpEnrolled(userId) -> "
                + "boolean required when enableTotp = true");
        }
        if (typeof opts.totpVerify !== "function") {
            throw new Error("auth-flows.init: totpVerify(user, code) -> "
                + "boolean required when enableTotp = true");
        }
    }

    _state.stateSecretHex = bytesToHex(opts.stateSecret);
    _state.emailSend      = opts.emailSend;
    _state.templates      = opts.templates;
    _state.userFindByEmail      = opts.userFindByEmail;
    _state.userGet              = opts.userGet;
    _state.userCreate           = opts.userCreate;
    _state.userSetPassword      = opts.userSetPassword;
    _state.userSetEmail         = opts.userSetEmail;
    _state.userSetEmailVerified = opts.userSetEmailVerified;
    _state.onLogin              = opts.onLogin;
    _state.onLogout             = opts.onLogout || null;
    _state.enableTotp           = opts.enableTotp === true;
    _state.userTotpEnrolled     = opts.userTotpEnrolled || null;
    _state.totpVerify           = opts.totpVerify || null;
    _state.totpPendingTtl       = opts.totpPendingTtl || _state.totpPendingTtl;
    _state.totpPendingRedirect  = opts.totpPendingRedirect
                                  || _state.totpPendingRedirect;
    _state.verifyTtl       = opts.verifyTtl       || _state.verifyTtl;
    _state.resetTtl        = opts.resetTtl        || _state.resetTtl;
    _state.magicLinkTtl    = opts.magicLinkTtl    || _state.magicLinkTtl;
    _state.emailChangeTtl  = opts.emailChangeTtl  || _state.emailChangeTtl;
    _state.prefix          = opts.prefix          || _state.prefix;
    _state.verifyRedirect  = opts.verifyRedirect  || _state.verifyRedirect;
    _state.loginRedirect   = opts.loginRedirect   || _state.loginRedirect;
    if (opts.enumerationSafe     !== undefined) _state.enumerationSafe     = opts.enumerationSafe;
    if (opts.magicLinkAutoSignup !== undefined) _state.magicLinkAutoSignup = opts.magicLinkAutoSignup;
    if (opts.requireVerifiedEmail !== undefined) _state.requireVerifiedEmail = opts.requireVerifiedEmail;

    db.batch(() => {
        const stmts = SCHEMA.split(";");
        for (let i = 0; i < stmts.length; i++) {
            const s = stmts[i].trim();
            if (s.length > 0) db.exec(s);
        }
    });

    _state.initialized = true;
}

function routes(app) {
    if (!_state.initialized) {
        throw new Error("auth-flows.routes: call init() first");
    }
    registerRoutes(app);
}

function sendVerifyEmail(user, verifyUrlPrefix) {
    if (!_state.initialized) throw new Error("auth-flows: call init() first");
    if (!user || !(user.id || user.user_id)) {
        throw new Error("auth-flows.sendVerifyEmail: user with id required");
    }
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.verify_email, _state.verifyTtl);
    const verifyUrl = (verifyUrlPrefix || "") + _state.prefix
        + "/verify?token=" + token;
    sendEmail(user.email, "welcome", { user, verify_url: verifyUrl, token });
}

function sendPasswordReset(email, resetUrlPrefix) {
    if (!_state.initialized) throw new Error("auth-flows: call init() first");
    if (!isEmailIsh(email)) throw new Error("auth-flows.sendPasswordReset: invalid email");
    const user = _state.userFindByEmail(email);
    if (!user) return;
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.password_reset, _state.resetTtl);
    const link = (resetUrlPrefix || "") + _state.prefix
        + "/password-reset/confirm?token=" + token;
    sendEmail(email, "password_reset", { user, link, token });
}

function sendMagicLink(email, magicUrlPrefix) {
    if (!_state.initialized) throw new Error("auth-flows: call init() first");
    if (!isEmailIsh(email)) throw new Error("auth-flows.sendMagicLink: invalid email");
    let user = _state.userFindByEmail(email);
    if (!user) {
        if (!_state.magicLinkAutoSignup) return;
        const uid = _state.userCreate(email, null);
        user = _state.userGet(uid);
    }
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.magic_link, _state.magicLinkTtl);
    const link = (magicUrlPrefix || "") + _state.prefix
        + "/magic-link/consume?token=" + token;
    sendEmail(email, "magic_link", { user, link, token });
}

const _test = {
    issueToken,
    consumeToken,
    parseToken,
    markTokenUsed,
    tokenAlreadyUsed,
    renderTemplate,
    gcExpired,
    isEmailIsh,
    parseBody,
    ACTIONS,
    reset: () => {
        _state.stateSecretHex = null;
        _state.emailSend      = null;
        _state.templates      = {};
        _state.userFindByEmail      = null;
        _state.userGet              = null;
        _state.userCreate           = null;
        _state.userSetPassword      = null;
        _state.userSetEmail         = null;
        _state.userSetEmailVerified = null;
        _state.onLogin              = null;
        _state.onLogout             = null;
        _state.enableTotp           = false;
        _state.userTotpEnrolled     = null;
        _state.totpVerify           = null;
        _state.totpPendingRedirect  = null;
        _state.initialized          = false;
    },
};

const authFlows = {
    init, routes,
    sendVerifyEmail, sendPasswordReset, sendMagicLink,
    _test,
};
export { authFlows };
