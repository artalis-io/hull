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

import { crypto }   from "hull:crypto";
import { envelope } from "hull:crypto:envelope";
import { pwned }    from "hull:web:pwned";
import { auditLog } from "hull:web:middleware:audit-log";
import { db }       from "hull:db";
import { time }     from "hull:time";
import { json }     from "hull:json";

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
    // Hardening: account lockout. See the Lua module for the design.
    maxFailedLogins:        5,
    lockoutDuration:        15 * 60,
    // Hardening: pwned-password check (opt-in). Apps must add
    // api.pwnedpasswords.com to manifest.hosts.
    checkPwnedPasswords:    false,
    pwnedEndpoint:          null,
    // Sign-in events (opt-in). When signInLog = true, auth-flows
    // records every login / password reset / email change via
    // hull/web/middleware/audit-log. Pair with onNewDevice for
    // the "you signed in from a new device" email; pair with
    // onPasswordReset to revoke sessions (typically
    // `(req, res, user) => session.destroyAll(user.id)`).
    signInLog:              false,
    onNewDevice:            null,
    onPasswordReset:        null,
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

CREATE TABLE IF NOT EXISTS _hull_auth_login_attempts (
    user_id        TEXT PRIMARY KEY,
    failed_count   INTEGER NOT NULL DEFAULT 0,
    last_failed_at INTEGER,
    locked_until   INTEGER
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
    // Sent to the OLD address on email-change so the old-address
    // holder can cancel a hostile change within TTL.
    email_change_revoke: "email_change_revoke",
    // Pending 2FA token. Multi-use within TTL (allows retry on
    // typo); burned on a successful code verify.
    totp_pending:   "totp_pending",
};

// Kept local (not aliased to crypto.hexEncode) because state
// secrets may contain code points >= 0x80; crypto.hexEncode for
// string input goes through JS_ToCStringLen which UTF-8-inflates
// them, breaking round-trips. The cap-layer helper is correct
// for ArrayBuffer/Uint8Array; use it for those.
function bytesToHex(s) {
    let h = "";
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i) & 0xff;
        h += (c < 16 ? "0" : "") + c.toString(16);
    }
    return h;
}

// Signature framing lives in hull:crypto:envelope; this wrapper
// just builds the payload and adds optional extra fields. The
// action / expiry / single-use checks remain in parseToken /
// consumeToken since they're auth-flows-specific.
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
    return envelope.sign(payload, _state.stateSecretHex);
}

// Verify signature + action + expiry WITHOUT marking the token
// used. Signature framing comes from hull:crypto:envelope; the
// action and expiry checks are auth-flows-specific.
function parseToken(token, expectedAction) {
    const r = envelope.verify(token, _state.stateSecretHex);
    if (!r[0]) return [null, r[1]];
    const env = r[0];
    if (env.action !== expectedAction) return [null, "wrong action"];
    if (typeof env.exp !== "number" || time.now() >= env.exp) {
        return [null, "expired"];
    }
    return [env, null];
}

function markTokenUsed(token, exp) {
    const tokenHash = crypto.sha256(token);
    const rc = db.insertIfAbsent(
        "_hull_auth_used_tokens",
        ["token_hash"],
        ["token_hash", "used_at", "expires_at"],
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
    db.exec(
        "DELETE FROM _hull_auth_login_attempts "
        + "WHERE (locked_until IS NULL OR locked_until < ?) "
        + "  AND (last_failed_at IS NULL OR last_failed_at < ?)",
        [now, now - 86400]);
}

// ── Lockout helpers ─────────────────────────────────────────────
// Mirror of the Lua module. See its header for the design.

function lockoutRemaining(userIdStr) {
    const rows = db.query(
        "SELECT locked_until FROM _hull_auth_login_attempts WHERE user_id = ?",
        [userIdStr]);
    if (!rows || rows.length === 0) return 0;
    const lu = rows[0].locked_until;
    if (!lu) return 0;
    const now = time.now();
    return lu > now ? (lu - now) : 0;
}

function bumpFailedLogin(userIdStr) {
    const now = time.now();
    db.exec(
        "INSERT INTO _hull_auth_login_attempts "
        + "(user_id, failed_count, last_failed_at, locked_until) "
        + "VALUES (?, 1, ?, NULL) "
        + "ON CONFLICT(user_id) DO UPDATE SET "
        + "  failed_count = failed_count + 1, "
        + "  last_failed_at = ?, "
        + "  locked_until = CASE WHEN failed_count + 1 >= ? "
        + "                      THEN ? + ? ELSE NULL END",
        [userIdStr, now, now,
         _state.maxFailedLogins, now, _state.lockoutDuration]);
}

function clearFailedLogins(userIdStr) {
    db.exec("DELETE FROM _hull_auth_login_attempts WHERE user_id = ?",
            [userIdStr]);
}

// ── Pwned-password check (opt-in) ──────────────────────────────
// Statically imports hull:web:pwned (it's a transitive dep of
// hull/web/auth-flows in the module registry, so the resolver
// admits it for every app declaring auth-flows even if they
// don't enable the check). Returns Promise<bool>: true if
// pwned (caller should reject), false otherwise (incl. fail-
// open on HIBP outage).
async function checkPwned(password) {
    if (!_state.checkPwnedPasswords) return false;
    return pwned.check(password,
        _state.pwnedEndpoint ? { endpoint: _state.pwnedEndpoint } : undefined);
}

// ── Sign-in event emit + finish-login helper ──────────────────
//
// Mirror of the Lua emit_event / finish_login. No-op when
// signInLog isn't enabled.
function emitEvent(uid, kind, req, opts) {
    if (!_state.signInLog) return;
    try { auditLog.record(uid, kind, req, opts); }
    catch (_e) { /* don't let the log break the login */ }
}

function finishLogin(req, res, user, factors) {
    const uid = userId(user);
    if (_state.signInLog && _state.onNewDevice) {
        let isNew = false;
        try { isNew = auditLog.isNewDevice(uid, req); }
        catch (_e) { isNew = false; }
        if (isNew) {
            try { _state.onNewDevice(req, res, user); } catch (_e) {}
        }
    }
    emitEvent(uid, "login", req, { metadata: { factors: factors } });
    _state.onLogin(req, res, user);
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

// Tolerate either `user.id` (canonical) or `user.user_id` (legacy)
// on app-supplied user objects. Lua mirrors this with `user_uid`.
function userId(user) {
    if (!user || typeof user !== "object") return null;
    return user.id || user.user_id || null;
}

function originFor(req) {
    const proto = (req.headers && req.headers["x-forwarded-proto"]) || "http";
    const host  = (req.headers && req.headers["x-forwarded-host"])
                  || (req.headers && req.headers.host)
                  || "localhost";
    return proto + "://" + host;
}

// ── Route handlers ─────────────────────────────────────────────────

async function handleRegister(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    if (typeof body.password !== "string" || body.password.length < 8) {
        return res.status(400).json({ error: "password too short" });
    }
    // Pwned check runs BEFORE userFindByEmail so the same error
    // returns regardless of whether the email already exists.
    if (await checkPwned(body.password)) {
        return res.status(400).json({
            error: "password appears in known data breaches; choose another",
        });
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

// POST /auth/verify/resend { email } — enumeration-safe re-issue
// of the welcome / verify email for unverified users.
function handleVerifyResend(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    const user = _state.userFindByEmail(body.email);
    if (!user || user.email_verified) return genericOk(res);
    const uid = userId(user);
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
    // Lockout check before password check: a locked account
    // shouldn't leak whether the attempted password was right
    // via response timing.
    if (user) {
        const remain = lockoutRemaining(userId(user));
        if (remain > 0) {
            res.header("Retry-After", String(remain));
            return res.status(429).json({
                error: "too many failed attempts",
                retry_after: remain,
            });
        }
    }
    if (!user || !user.password_hash
        || !crypto.verifyPassword(body.password, user.password_hash)) {
        if (user) bumpFailedLogin(userId(user));
        return res.status(401).json({ error: "invalid credentials" });
    }
    if (_state.requireVerifiedEmail && !user.email_verified) {
        return res.status(403).json({ error: "email not verified" });
    }
    clearFailedLogins(userId(user));
    if (_state.enableTotp && _state.userTotpEnrolled(userId(user))) {
        return startTotpPending(req, res, user);
    }
    finishLogin(req, res, user, "password");
}

function handleLogout(req, res) {
    // user_id isn't known here without inspecting the session —
    // app's responsibility. Apps that want a "logout" event can
    // call auditLog.record inside their onLogout callback.
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
    finishLogin(req, res, user, "magic_link");
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
    finishLogin(req, res, user, "password+totp");
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

async function handlePasswordResetConfirm(req, res) {
    const body = parseBody(req);
    if (typeof body.password !== "string" || body.password.length < 8) {
        return res.status(400).json({ error: "password too short" });
    }
    if (await checkPwned(body.password)) {
        return res.status(400).json({
            error: "password appears in known data breaches; choose another",
        });
    }
    const result = consumeToken(body.token, ACTIONS.password_reset);
    if (!result[0]) {
        return res.status(400).json({
            error: "reset failed: " + (result[1] || "?") });
    }
    const user = _state.userGet(result[0].sub);
    if (!user) return res.status(400).json({ error: "reset failed" });
    _state.userSetPassword(result[0].sub, crypto.hashPassword(body.password));
    // A successful reset demonstrates email control; clear any
    // outstanding lockout so the new password works immediately.
    clearFailedLogins(result[0].sub);
    // Audit + app-side session revocation. Recommended onPasswordReset
    // body: `(req, res, user) => session.destroyAll(user.id)`.
    emitEvent(result[0].sub, "password_reset_completed", req);
    if (_state.onPasswordReset) {
        try { _state.onPasswordReset(req, res, user); } catch (_e) {}
    }
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
    db.upsert(
        "_hull_auth_pending_email_changes",
        ["user_id"],
        ["user_id", "new_email", "token_hash", "created_at", "expires_at"],
        [uid, body.new_email, tokenHash, now, now + _state.emailChangeTtl]);

    const user = _state.userGet(uid);
    const origin = originFor(req);
    const link = origin + _state.prefix
        + "/email-change/confirm?token=" + token;
    sendEmail(body.new_email, "email_change", {
        user, link, token, new_email: body.new_email,
    });
    // Defense in depth: notify the OLD address with a revoke link
    // if templates.email_change_notify is provided.
    if (_state.templates.email_change_notify) {
        const revokeTok = issueToken(uid, ACTIONS.email_change_revoke,
            _state.emailChangeTtl);
        const revokeUrl = origin + _state.prefix
            + "/email-change/revoke?token=" + revokeTok;
        sendEmail(user.email, "email_change_notify", {
            user, revoke_url: revokeUrl, revoke_token: revokeTok,
            new_email: body.new_email,
        });
    }
    res.json({ ok: true });
}

// GET /auth/email-change/revoke?token=... — old-address holder
// cancels a pending email change. Single-use via the shared
// used-tokens table.
function handleEmailChangeRevoke(req, res) {
    const token = req.query && req.query.token;
    const result = consumeToken(token, ACTIONS.email_change_revoke);
    if (!result[0]) {
        return res.status(400).html("revoke failed: " + (result[1] || "?"));
    }
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            [result[0].sub]);
    emitEvent(result[0].sub, "email_change_revoked", req,
              { metadata: { by: "old_address" } });
    gcExpired();
    res.html("Email change canceled.");
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
    const oldEmail = user.email;
    _state.userSetEmail(env.sub, env.new_email);
    _state.userSetEmailVerified(env.sub, true);
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            [env.sub]);
    emitEvent(env.sub, "email_changed", req,
              { metadata: { old_email: oldEmail, new_email: env.new_email } });
    gcExpired();
    res.redirect(_state.verifyRedirect);
}

function registerRoutes(app) {
    const p = _state.prefix;
    app.post(p + "/register",                 handleRegister);
    app.get (p + "/verify",                   handleVerify);
    app.post(p + "/verify/resend",            handleVerifyResend);
    app.post(p + "/login",                    handleLogin);
    app.post(p + "/logout",                   handleLogout);
    app.post(p + "/magic-link",               handleMagicLink);
    app.get (p + "/magic-link/consume",       handleMagicLinkConsume);
    app.post(p + "/password-reset/request",   handlePasswordResetRequest);
    app.post(p + "/password-reset/confirm",   handlePasswordResetConfirm);
    app.post(p + "/email-change",             handleEmailChange);
    app.get (p + "/email-change/confirm",     handleEmailChangeConfirm);
    app.get (p + "/email-change/revoke",      handleEmailChangeRevoke);
    // Registered unconditionally; the handler returns 404 when
    // enableTotp is false (clearer than a route-level 404).
    app.post(p + "/totp-verify",              handleTotpVerify);
}

// ── Public API ─────────────────────────────────────────────────────

/**
 * Build a turnkey adapter for the 6 userX callbacks against a
 * "standard" users-table schema. Pass the result as opts.users
 * to init() to skip the per-app DB-wrapper boilerplate. Apps
 * with a custom schema either override single callbacks
 * (opts.userCreate wins over opts.users.create) or skip the
 * adapter.
 *
 * DB-backend-agnostic — issues standard INSERT / UPDATE / SELECT
 * via the `db` module with no SQLite-specific syntax. Works on
 * whatever backend hull/db is wired to (SQLite today, Postgres
 * planned).
 *
 * Default schema (portable across SQLite + Postgres):
 *   CREATE TABLE users (
 *       id TEXT PRIMARY KEY, email TEXT NOT NULL UNIQUE,
 *       password_hash TEXT, email_verified INTEGER NOT NULL DEFAULT 0,
 *       created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL
 *   )
 */
function standardUsers(opts) {
    opts = opts || {};
    const tbl = opts.table || "users";
    const idGen = opts.idGen || (() => crypto.hexEncode(crypto.random(16)));

    function row(r) {
        if (!r) return null;
        return {
            id:             r.id,
            email:          r.email,
            password_hash:  r.password_hash,
            email_verified: r.email_verified === 1,
        };
    }

    return {
        findByEmail(email) {
            const rows = db.query(
                "SELECT * FROM " + tbl + " WHERE email = ?", [email]);
            return rows && rows[0] ? row(rows[0]) : null;
        },
        get(id) {
            const rows = db.query(
                "SELECT * FROM " + tbl + " WHERE id = ?", [id]);
            return rows && rows[0] ? row(rows[0]) : null;
        },
        create(email, pwhash) {
            const id = idGen();
            const now = time.now();
            db.exec(
                "INSERT INTO " + tbl
                + " (id, email, password_hash, email_verified, "
                + "  created_at, updated_at) "
                + "VALUES (?, ?, ?, 0, ?, ?)",
                [id, email, pwhash, now, now]);
            return id;
        },
        setPassword(id, pwhash) {
            db.exec(
                "UPDATE " + tbl
                + " SET password_hash = ?, updated_at = ? WHERE id = ?",
                [pwhash, time.now(), id]);
        },
        setEmail(id, email) {
            db.exec(
                "UPDATE " + tbl
                + " SET email = ?, updated_at = ? WHERE id = ?",
                [email, time.now(), id]);
        },
        setEmailVerified(id, verified) {
            db.exec(
                "UPDATE " + tbl
                + " SET email_verified = ?, updated_at = ? WHERE id = ?",
                [verified ? 1 : 0, time.now(), id]);
        },
    };
}

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
    // opts.users (typically from authFlows.standardUsers(...))
    // bulk-fills the 6 callbacks; explicit opts.userX still wins.
    // Adapter keys are short ("findByEmail", "create", etc.) so a
    // plain `users.create` reads naturally at the call site.
    const adapterKeys = {
        userFindByEmail:      "findByEmail",
        userGet:              "get",
        userCreate:           "create",
        userSetPassword:      "setPassword",
        userSetEmail:         "setEmail",
        userSetEmailVerified: "setEmailVerified",
    };
    if (opts.users && typeof opts.users === "object") {
        for (const k in adapterKeys) {
            if (opts[k] === undefined
                && typeof opts.users[adapterKeys[k]] === "function") {
                opts[k] = opts.users[adapterKeys[k]];
            }
        }
    }
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
    _state.maxFailedLogins      = opts.maxFailedLogins
                                  || _state.maxFailedLogins;
    _state.lockoutDuration      = opts.lockoutDuration
                                  || _state.lockoutDuration;
    _state.checkPwnedPasswords  = opts.checkPwnedPasswords === true;
    _state.pwnedEndpoint        = opts.pwnedEndpoint || null;
    _state.signInLog            = opts.signInLog === true;
    _state.onNewDevice          = opts.onNewDevice || null;
    _state.onPasswordReset      = opts.onPasswordReset || null;
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
    const uid = userId(user);
    if (!uid) throw new Error("auth-flows.sendVerifyEmail: user with id required");
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
        _state.checkPwnedPasswords  = false;
        _state.pwnedEndpoint        = null;
        _state.maxFailedLogins      = 5;
        _state.lockoutDuration      = 15 * 60;
        _state.signInLog            = false;
        _state.onNewDevice          = null;
        _state.onPasswordReset      = null;
        _state.initialized          = false;
    },
};

const authFlows = {
    init, routes, standardUsers,
    sendVerifyEmail, sendPasswordReset, sendMagicLink,
    _test,
};
export { authFlows };
