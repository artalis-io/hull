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
import { pwned }     from "hull:web:pwned";
import { auditLog }  from "hull:web:middleware:audit-log";
import { log }       from "hull:log";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { _request }  from "hull:web:_request";
import { internal as dbInternal } from "hull:db:_internal";
const db = dbInternal.connection();
import { time }     from "hull:time";
import { json }     from "hull:json";

const _state = {
    stateSecret:      null,
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
    // Per (account, client IP); the account-wide count below has a much
    // higher threshold (see the Lua module for the reasoning).
    maxFailedLogins:        5,
    maxFailedLoginsPerAccount: 50,
    lockoutDuration:        15 * 60,
    // Hardening: pwned-password check (opt-in). Apps must add
    // api.pwnedpasswords.com to manifest.hosts.
    checkPwnedPasswords:    false,
    pwnedEndpoint:          null,
    // Flow-completion audit events (opt-in). When signInLog = true,
    // auth-flows records password_reset_completed / email_change_revoked
    // / email_changed via hull/web/middleware/audit-log. Pair
    // with onPasswordReset to revoke sessions (typically
    // `(req, res, user) => session.destroyAll(user.id)`).
    //
    // LOGIN events and new-device detection are NOT emitted here
    // anymore - they move to hull/web/middleware/session's
    // loginHandler factory (auditLog + onNewDevice opts), so a
    // single seam covers both password and OAuth logins.
    signInLog:              false,
    onPasswordReset:        null,
    // Login rate limit (opt-in). When loginRatelimit is truthy, a
    // hull/web/middleware/ratelimit middleware is installed on POST
    // /auth/login + /magic-link + /password-reset/request BEFORE the
    // handlers. Defaults to 20 requests per IP per 5 minutes. Pass an
    // object to override: { limit, window, key }. Apps with their own
    // upstream rate-limiter should leave this off.
    loginRatelimit:         false,
    // Per-recipient email send rate limit (ON by default). Gate
    // inside sendEmail; blocked sends are silently dropped so the
    // response stays enumeration-safe. Defends against the
    // attacker-chosen-recipient email-storm class on /auth/email-
    // change, /auth/magic-link, /auth/password-reset/request,
    // /auth/verify/resend, /auth/register. loginRatelimit (per-IP)
    // is orthogonal - a botnet defeats per-IP but not per-recipient.
    //
    // Shape: { limit: N, window: SECONDS }. Pass false to disable.
    // In-memory sliding window; resets on restart. Bounded to
    // emailRateLimitMaxEntries unique recipients.
    emailRateLimit:         { limit: 3, window: 900 },
    emailRateLimitMaxEntries: 10000,
    verifyRedirect:      "/",
    // GET /verify renders a default form (confirm the password, or set a new
    // one). Set this to an app page to render your own: it is redirected to
    // with ?token=... appended, and POSTs {token, password | new_password}
    // to <prefix>/verify. See handleVerify.
    verifyFormRedirect:  null,
    // `(userId) => ...` removing a TOTP enrolment (typically totp.disable),
    // called when the mailbox holder sets the password of an account that was
    // not verified yet. See dropPreverifyTotp.
    totpDisable:         null,
    // `(req, user) => true` when the request proves a recent sign-in of its
    // own (or for passwordless accounts): POST /email-change then needs no
    // current password. See handleEmailChange.
    emailChangeReauth:   null,
    loginRedirect:       "/",
    initialized:         false,
};

const SCHEMA = `
CREATE TABLE IF NOT EXISTS _hull_auth_used_tokens (
    token_hash  VARCHAR(255) PRIMARY KEY,
    used_at     INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS _hull_auth_pending_email_changes (
    user_id      VARCHAR(255) PRIMARY KEY,
    new_email    TEXT NOT NULL,
    token_hash   TEXT NOT NULL,
    created_at   INTEGER NOT NULL,
    expires_at   INTEGER NOT NULL,
    old_email    TEXT,
    confirmed_at INTEGER
);

CREATE TABLE IF NOT EXISTS _hull_auth_login_attempts (
    user_id        VARCHAR(255) PRIMARY KEY,
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

import { encoding } from "hull:encoding";

// Signature framing lives in hull:crypto:envelope; this wrapper
// just builds the payload and adds optional extra fields. The
// action / expiry / single-use checks remain in parseToken /
// consumeToken since they're auth-flows-specific.
// A reset token names the password it replaces: a short hash of the
// account's password_hash at issue time, checked again at confirm. Once the
// password changes, every other outstanding reset link - a leaked one
// included - stops working, instead of living out its resetTtl. Read through
// userFindByEmail, the lookup login itself relies on for password_hash, so
// issue and confirm see the same field.
// The account's email is part of it too: a link sent to the old mailbox
// stayed good after an email change (the password hash was unchanged), so
// whoever still reads that mailbox could reset the password.
function emailBinding(user) {
    const e = user && user.email;
    return encoding.hex.encode(crypto.sha256(
        typeof e === "string" ? e.toLowerCase() : "")).slice(0, 16);
}

function passwordBinding(user) {
    const h = user && user.password_hash;
    return encoding.hex.encode(crypto.sha256(
        (typeof h === "string" ? h : "") + "\0" + emailBinding(user))).slice(0, 16);
}

function resetTokenExtra(user) { return { pwb: passwordBinding(user) }; }

function resetBindingHolds(env, user) {
    const current = user && typeof user.email === "string"
        ? findByEmail(user.email) : null;
    return !!current && env.pwb === passwordBinding(current);
}

function issueToken(userId, action, ttl, extra) {
    const payload = {
        sub:    userId,
        action: action,
        exp:    time.now() + ttl,
        nonce:  crypto.randomToken(16),
    };
    if (extra) {
        for (const k in extra) {
            if (Object.prototype.hasOwnProperty.call(extra, k)) {
                payload[k] = extra[k];
            }
        }
    }
    return envelope.sign(payload, _state.stateSecret);
}

// Verify signature + action + expiry WITHOUT marking the token
// used. Signature framing comes from hull:crypto:envelope; the
// action and expiry checks are auth-flows-specific.
function parseToken(token, expectedAction) {
    const r = envelope.verify(token, _state.stateSecret);
    if (!r[0]) return [null, r[1]];
    const env = r[0];
    if (env.action !== expectedAction) return [null, "wrong action"];
    if (typeof env.exp !== "number" || time.now() >= env.exp) {
        return [null, "expired"];
    }
    return [env, null];
}

function markTokenUsed(token, exp) {
    const tokenHash = encoding.hex.encode(crypto.sha256(token));
    const rc = db.insertIfAbsent(
        "_hull_auth_used_tokens",
        ["token_hash"],
        ["token_hash", "used_at", "expires_at"],
        [tokenHash, time.now(), exp]);
    return rc > 0;
}

function tokenAlreadyUsed(token) {
    const tokenHash = encoding.hex.encode(crypto.sha256(token));
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

// Per-recipient email send rate limit. Sliding window keyed by
// lower-cased recipient. Blocked sends are dropped silently so
// the response shape stays enumeration-safe.
let _emailRl = new Map();
// See the Lua sibling: the next sweep runs past this size, so a map held
// over the cap by saturated buckets does not re-sweep for every new key.
let _emailRlSweepAt = 0;

function emailRateAllow(to) {
    const cfg = _state.emailRateLimit;
    if (!cfg || typeof cfg !== "object") return true;
    if (typeof to !== "string" || to === "") return true;
    const key = to.toLowerCase();
    const now = time.now();
    const window = cfg.window || 900;
    const limit = cfg.limit || 3;
    const cutoff = now - window;
    let bucket = _emailRl.get(key);
    if (!bucket) {
        bucket = { ts: [] };
        _emailRl.set(key, bucket);
        const max = _state.emailRateLimitMaxEntries || 10000;
        if (_emailRl.size > Math.max(_emailRlSweepAt, max)) {
            // Avoid `for (const [k, b] of _emailRl)` - QuickJS's
            // js_parse_destructuring_element has an MSan use-of-
            // uninitialized-value in its destructuring parser that
            // tripped the round-8 MSan job. The forEach form sidesteps
            // it without changing semantics.
            const kept = new Map();
            _emailRl.forEach((b, k) => {
                if (b.ts.some(t => t > cutoff)) kept.set(k, b);
            });
            _emailRl = kept;
            _emailRl.set(key, bucket);
            // Still over: a flood of distinct recipients, all inside the
            // window, so the sweep kept them all - the map outgrew the cap
            // and every new key paid an O(n) sweep. Drop the least recently
            // used down to 90% of the cap, so the next sweep is a tenth of
            // the cap away. The key just added is kept, and so is every
            // bucket AT its limit: dropping one reset it, so flooding other
            // addresses bought an attacker a fresh allowance against the
            // address they were blocked on. A saturated bucket costs `limit`
            // sends to create, so keeping them all stays bounded.
            if (_emailRl.size > max) {
                const target = Math.floor(max * 0.9);
                const order = [];
                _emailRl.forEach((b, k) => {
                    const live = b.ts.filter(t => t > cutoff).length;
                    if (k !== key && live < limit)
                        order.push({ k, t: b.ts.length ? b.ts[b.ts.length - 1] : 0 });
                });
                order.sort((x, y) => x.t - y.t);
                for (let i = 0; i < order.length && _emailRl.size > target; i++)
                    _emailRl.delete(order[i].k);
            }
            // Hard ceiling: past twice the cap, saturated buckets go too,
            // oldest first, down to the cap.
            if (_emailRl.size > 2 * max) {
                const sat = [];
                _emailRl.forEach((b, k) => {
                    if (k !== key)
                        sat.push({ k, t: b.ts.length ? b.ts[b.ts.length - 1] : 0 });
                });
                sat.sort((x, y) => x.t - y.t);
                for (let i = 0; i < sat.length && _emailRl.size > max; i++)
                    _emailRl.delete(sat[i].k);
            }
            _emailRlSweepAt = _emailRl.size + Math.max(1, Math.floor(max / 10));
        }
    }
    bucket.ts = bucket.ts.filter(t => t > cutoff);
    if (bucket.ts.length >= limit) return false;
    bucket.ts.push(now);
    return true;
}

// Round-9 MEDIUM-6: strict allowlist + optional userSanitize hook.
// See Lua sibling for the threat model.
const SAFE_USER_FIELDS = {
    id:             true,
    user_id:        true,
    email:          true,
    email_verified: true,
};

function stripUserSecrets(user) {
    if (!user || typeof user !== "object") return user;
    if (_state.userSanitize) {
        try {
            const sanitized = _state.userSanitize(user);
            // Round-11 HIGH-1: reject Promises explicitly. A Promise
            // is typeof === "object" and truthy, so the pre-fix
            // check accepted it as the sanitized user. The Promise
            // then got stuffed into the session payload as `{}`, and
            // downstream `req.ctx.user.id` was undefined → auth
            // checks degraded to anonymous. Round-10 review flagged
            // this; the round-10 fix only handled sync-throw + non-
            // table cases. Falling through to the allowlist on a
            // thenable is fail-safe (still returns a usable user
            // with the canonical fields).
            if (sanitized && typeof sanitized.then === "function") {
                log.warn("auth-flows: userSanitize returned a Promise "
                    + "(async callback); userSanitize MUST be sync. "
                    + "Falling back to strict allowlist.");
            } else if (sanitized && typeof sanitized === "object") {
                return sanitized;
            } else {
                log.warn("auth-flows: userSanitize returned non-object; "
                    + "falling back to strict allowlist");
            }
        } catch (_e) {
            log.warn("auth-flows: userSanitize threw; falling back to "
                + "strict allowlist");
        }
    }
    const out = {};
    for (const k in user) {
        if (SAFE_USER_FIELDS[k]) out[k] = user[k];
    }
    return out;
}

// Run fn after the response has gone. Issuing a token and sending its email
// happen only for SOME addresses (an existing account, or a new one), and an
// email send is a network round trip: done inline, response time said
// whether an account exists. Deferred onto the event loop, every outcome
// answers equally fast. Inline only where there is no loop to defer onto (an
// in-process test harness, where hull.sleep throws); a failure is logged, not
// thrown - the response is already sent.
//
// A hull.sleep made in the handler attaches to the request's connection: the
// response waited for it, then for fn's template and email send (an SMTP op
// fn started attached too) - for existing accounts only, a timing oracle
// (audit 8). So fn is queued, and the timer is armed from a promise job: the
// dispatcher runs the jobs a SYNCHRONOUS handler queued once its request is
// over, so that timer - and everything fn starts - belongs to no request.
// Every handler that defers is synchronous for that reason (handleRegister
// is unless checkPwnedPasswords makes it wait on HIBP; the Lua twin spawns a
// detached coroutine, which JS has no primitive for).
const _deferred = [];
let _deferArmed = false;

function runDeferred() {
    _deferArmed = false;
    const batch = _deferred.splice(0, _deferred.length);
    for (const fn of batch) {
        try { fn(); }
        catch (e) { log.warn("auth-flows: deferred email failed: " + String(e && e.message || e)); }
    }
}

function armDeferred() {
    let wait = null;
    try { wait = hull.sleep(1); } catch (_) { wait = null; }
    if (wait && typeof wait.then === "function") {
        wait.then(runDeferred, runDeferred);
        return;
    }
    runDeferred();
}

function afterResponse(fn) {
    _deferred.push(fn);
    if (_deferArmed) return;
    _deferArmed = true;
    Promise.resolve().then(armDeferred);
}

// An emailSend that returns a Promise (email.send is async) rejects on a
// failed send, and nothing observed it: every verification / reset / magic
// link was dropped without a log line. Its failure is logged here, as the Lua
// twin's pcall logs one.
function sendEmail(to, templateName, ctx) {
    if (!emailRateAllow(to)) return;
    if (ctx && typeof ctx === "object" && ctx.user
        && typeof ctx.user === "object") {
        ctx.user = stripUserSecrets(ctx.user);
    }
    const r = renderTemplate(templateName, ctx);
    const sent = _state.emailSend(to, r.subject, r.html, r.text);
    if (sent && typeof sent.then === "function") {
        try {
            sent.then(undefined, (e) => {
                log.warn("auth-flows: email send failed: "
                    + String(e && (e.message || e.code) || e));
            });
        } catch (_) { /* a thenable whose then throws: nothing to observe */ }
    }
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

function bumpFailedLogin(userIdStr, max) {
    const now = time.now();
    // Portable conditional upsert. The original used INSERT ... ON CONFLICT
    // DO UPDATE, which MySQL spells differently (ON DUPLICATE KEY UPDATE), so
    // do it in two portable steps that hold on every backend: an atomic
    // CASE-based UPDATE (standard SQL), then an INSERT only when no row matched
    // (first failure). No explicit transaction, so it composes safely even if
    // the login handler already runs inside one.
    //
    // The CASE mirrors the original (Round-9 HIGH-2): reset failed_count to 1
    // when a prior lockout window has expired but gc_expired hasn't pruned the
    // row yet, so a stale count doesn't trip a fresh lockout on the next bad
    // attempt (pre-fix the user got 1 attempt per 15min for the next 24h, not
    // 5); else increment and lock at the threshold.
    const updateSql =
        "UPDATE _hull_auth_login_attempts SET "
        + "  failed_count = CASE "
        + "    WHEN locked_until IS NOT NULL AND locked_until < ? THEN 1 "
        + "    ELSE failed_count + 1 "
        + "  END, "
        + "  last_failed_at = ?, "
        + "  locked_until = CASE "
        + "    WHEN locked_until IS NOT NULL AND locked_until < ? THEN NULL "
        + "    WHEN failed_count + 1 >= ? THEN ? + ? "
        + "    ELSE NULL "
        + "  END "
        + "WHERE user_id = ?";
    const updateArgs = [
        now,                // failed_count CASE: window-expired check
        now,                // last_failed_at
        now,                // locked_until CASE: window-expired check
        max, now, _state.lockoutDuration,
        userIdStr];
    if (db.exec(updateSql, updateArgs) > 0) return;
    // No existing row: first failure for this user. INSERT; if a concurrent
    // request won the insert race (duplicate PK), fall back to the atomic
    // UPDATE so the increment still lands.
    try {
        db.exec(
            "INSERT INTO _hull_auth_login_attempts "
            + "(user_id, failed_count, last_failed_at, locked_until) "
            + "VALUES (?, 1, ?, NULL)",
            [userIdStr, now]);
    } catch (e) {
        db.exec(updateSql, updateArgs);
    }
}

function clearFailedLogins(userIdStr) {
    db.exec("DELETE FROM _hull_auth_login_attempts WHERE user_id = ?",
            [userIdStr]);
}

// The lockout rows a login touches: (account, client IP), keyed
// `<user_id> \x1f <ip>` in the same column, and the account-wide one.
const IP_SEP = "\x1f";
function attemptIpKey(uid, req) {
    return uid + IP_SEP + (_request.limitKey(_request.clientIp(req, _state.trustProxy)) || "_anon");
}

// Every row for an account, after a password reset proves control of it.
function clearAllFailedLogins(uid) {
    const pat = String(uid).replace(/[!%_]/g, "!$&") + IP_SEP + "%";
    db.exec("DELETE FROM _hull_auth_login_attempts "
            + "WHERE user_id = ? OR user_id LIKE ? ESCAPE '!'",
            [uid, pat]);
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

// Hand off to the app-supplied onLogin (typically
// session.loginHandler) with the factors metadata in the ctx 4th arg.
// The audit row + new-device hook are now emitted by
// session.loginHandler - see hull:web:middleware:session's
// auditLog/onNewDevice opts. That single seam covers OAuth too.
function finishLogin(req, res, user, factors) {
    // Round-8 LOW-10: scrub password_hash before handing user to
    // the app callback. session.loginHandler stashes the user blob
    // in the session payload, which is JSON-encoded + persisted +
    // re-read on every load; a leaked hash would persist on disk
    // and surface in session.listForUser output.
    // Returned, and every caller returns it: an async onLogin (one that sets
    // the session cookie after an await) is then awaited by the dispatcher
    // instead of the request ending empty, without its cookie.
    return _state.onLogin(req, res, stripUserSecrets(user), { factors: factors });
}

function parseBody(req) {
    const body = req.body || "";
    if (body.length === 0) return {};
    if (isJsonCt(req)) {
        try { const t = json.decode(body); return (t && typeof t === "object") ? t : {}; }
        catch (_e) { return {}; }
    }
    const out = {};
    const pairs = body.split("&");
    for (let i = 0; i < pairs.length; i++) {
        const eq = pairs[i].indexOf("=");
        if (eq >= 0) {
            const k = pairs[i].substring(0, eq);
            out[k] = encoding.url.decode(pairs[i].substring(eq + 1), { form: true });
        }
    }
    return out;
}

function isEmailIsh(s) {
    if (typeof s !== "string") return false;
    if (s.length < 3 || s.length > 254) return false;
    // Round-9 MEDIUM-8: reject control chars (< 0x20 or 0x7f). See
    // Lua sibling for the rate-limit-bypass + SMTP-truncation attack.
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 0x20 || c === 0x7f) return false;
    }
    // One address, exactly: a single '@' and none of the characters that
    // separate or quote addresses (see the Lua sibling).
    if (/[,;<>"()\s]/.test(s)) return false;
    const at = s.indexOf("@");
    if (at < 1 || at === s.length - 1) return false;
    if (s.indexOf("@", at + 1) >= 0) return false;
    const dot = s.indexOf(".", at);
    if (dot < 0 || dot === at + 1 || dot === s.length - 1) return false;
    return true;
}

function genericOk(res) { res.json({ ok: true }); }

// The pending email change of a user, deleted whenever the password is reset
// or voided: started from a hijacked session, its confirm link otherwise still
// moved the account to the attacker's address after the owner's reset. A
// CONFIRMED change's row stays: it is what lets the old address revoke it.
function dropPendingEmailChange(uid) {
    db.exec("DELETE FROM _hull_auth_pending_email_changes "
            + "WHERE user_id = ? AND confirmed_at IS NULL", [uid]);
}

// A callback whose answer gates authentication must answer synchronously:
// a Promise is truthy, so an async totpVerify passed every code (2FA bypass)
// and an async userTotpEnrolled sent everyone to the 2FA step. A thenable is
// refused (its rejection observed, so it is not unhandled) - the caller fails
// closed with a 500.
function isThenable(v) {
    if (v && typeof v.then === "function") {
        try { v.then(() => {}, () => {}); } catch (_) { /* not a real promise */ }
        return true;
    }
    return false;
}

function totpEnrolled(user) {
    const v = _state.userTotpEnrolled(userId(user));
    if (isThenable(v)) {
        log.error("auth-flows: userTotpEnrolled returned a Promise; it must be synchronous");
        return null;
    }
    return !!v;
}

// The user lookups answer synchronously: an async userFindByEmail returned a
// truthy Promise, so resend / magic-link / reset mailed a sub=null token to
// any address submitted. A thenable is a misconfiguration: throw (the
// dispatcher answers 500).
function findByEmail(email) {
    const u = _state.userFindByEmail(email);
    if (isThenable(u)) {
        throw new Error("auth-flows: userFindByEmail returned a Promise; it must be synchronous");
    }
    return u;
}

function getUser(id) {
    const u = _state.userGet(id);
    if (isThenable(u)) {
        throw new Error("auth-flows: userGet returned a Promise; it must be synchronous");
    }
    return u;
}

// onPasswordReset may be async (a revocation that awaits a db.async call):
// it is awaited, so its failure is caught and logged here rather than left
// as an unobserved rejection. Logged, not swallowed: the recommended body
// revokes every session, so an operator must see a failure.
async function runOnPasswordReset(req, res, user) {
    if (!_state.onPasswordReset) return;
    try {
        await _state.onPasswordReset(req, res, user);
    } catch (e) {
        log.warn("auth-flows: onPasswordReset threw: " + (e && e.message ? e.message : e));
    }
}

// A TOTP enrolment made before the address was verified may be the
// pre-registrant's (see the Lua sibling, drop_preverify_totp). Removed via
// totpDisable when configured; otherwise an existing one is only logged.
async function dropPreverifyTotp(uid) {
    if (_state.totpDisable) {
        try { await _state.totpDisable(uid); }
        catch (e) { log.warn("auth-flows: totpDisable threw: " + (e && e.message ? e.message : e)); }
        return;
    }
    if (_state.enableTotp) {
        let enrolled = false;
        try { enrolled = _state.userTotpEnrolled(uid); } catch (_) { enrolled = false; }
        if (isThenable(enrolled)) enrolled = true;
        if (enrolled) {
            log.warn("auth-flows: account " + String(uid) + " has a TOTP enrolment "
                + "made before its email was verified and no totpDisable hook is "
                + "configured; remove it in onPasswordReset (pass totpDisable: totp.disable)");
        }
    }
}

// The mailbox holder chose newHash for a not-yet-verified account. What a
// pre-registrant could have attached goes first; verified is set LAST, so a
// failure part way never leaves a verified account with the old password.
async function replaceUnverifiedCredentials(req, res, user, uid, newHash) {
    _state.userSetPassword(uid, newHash);
    dropPendingEmailChange(uid);
    await dropPreverifyTotp(uid);
    clearAllFailedLogins(uid);
    await runOnPasswordReset(req, res, user);
    _state.userSetEmailVerified(uid, true);
    user.email_verified = true;
}

// Apply the three security headers that every auth-flow HTML
// response wants: clickjacking, cache, referrer. No opt-out because
// there's no legitimate reason to frame your own auth flow, cache
// it client-side, or leak the referrer when navigating off it.
// Set BEFORE writing the body so res.html still owns content-type.
function secureHtml(res) {
    res.header("X-Frame-Options", "DENY");
    res.header("Cache-Control", "no-store");
    res.header("Referrer-Policy", "strict-origin-when-cross-origin");
    return res;
}

// Tolerate either `user.id` (canonical) or `user.user_id` (legacy)
// on app-supplied user objects. Lua mirrors this with `user_uid`.
function userId(user) {
    if (!user || typeof user !== "object") return null;
    // `??`, not `||`: a user whose id is 0 is a user (Lua treats 0 as true).
    return user.id ?? user.user_id ?? null;
}

// Is the account's address verified? Only `true` or a non-zero number (an
// adapter returning raw rows hands back 0 / 1). Plain truthiness took a
// string column ("0", "false", "f") for verified: login skipped
// requireVerifiedEmail and a magic link signed a pre-registrant's account in.
// Same test as the Lua sibling's is_verified.
function isVerified(user) {
    const v = user && user.email_verified;
    return v === true || (typeof v === "number" && v !== 0);
}

// The account's stored password hash, read the way /login reads it: through
// userFindByEmail, whose contract carries password_hash. userGet need not
// return it (keeping the hash out of the model is a common habit), and reading
// it there made a missing field look like "no password". Returns the hash,
// null when the account has none, or false when it cannot be determined -
// callers then treat the account as having one (fail closed).
function storedPasswordHash(user) {
    let h = user && user.password_hash;
    if (typeof h === "string" && h !== "") return h;
    if (!user || typeof user !== "object" || typeof user.email !== "string") return false;
    const found = findByEmail(user.email);
    if (!found || typeof found !== "object"
        || String(userId(found)) !== String(userId(user))) return false;
    h = found.password_hash;
    if (typeof h === "string" && h !== "") return h;
    return null;
}

// Password bounds: 8..256 characters counted by codepoint (as the Lua twin
// counts them; .length counts UTF-16 units), plus a UTF-8 byte cap that keeps
// the PBKDF2 input bounded. `min` is false where only the upper bound applies
// (a login attempt).
const PW_MIN = 8, PW_MAX = 256, PW_MAX_BYTES = 1024;
function passwordLenOk(pw, min) {
    if (typeof pw !== "string" || pw.length > PW_MAX_BYTES) return false;
    let chars = 0, bytes = 0;
    for (const ch of pw) {
        const cp = ch.codePointAt(0);
        chars++;
        bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    }
    return chars <= PW_MAX && bytes <= PW_MAX_BYTES && (min === false || chars >= PW_MIN);
}

// The request's host, strictly: X-Forwarded-Host only behind a trusted
// proxy (a direct client sets any header it likes), its first entry, and
// nothing but a hostname or bracketed IPv6 literal plus an optional numeric
// port. Returns [host, port|null] - or null. Anything else (userinfo, a path,
// percent-encoding) is refused, never trimmed into shape.
function requestHost(headers) {
    let raw = (_state.trustProxy && headers["x-forwarded-host"]) || headers.host;
    if (typeof raw !== "string") return null;
    const comma = raw.indexOf(",");
    if (comma >= 0) raw = raw.substring(0, comma);
    raw = raw.trim();
    const m = /^(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+)(?::(\d{1,5}))?$/.exec(raw);
    if (!m) return null;
    if (m[2] !== undefined && Number(m[2]) > 65535) return null;
    return [m[1], m[2] === undefined ? null : m[2]];
}

// X-Forwarded-Proto behind a trusted proxy, and only "http" / "https".
function requestProto(headers, dflt) {
    if (_state.trustProxy) {
        const p = headers["x-forwarded-proto"];
        if (p === "http" || p === "https") return p;
    }
    return dflt;
}

function originFor(req) {
    if (_state.publicOrigin) return _state.publicOrigin;
    const headers = (req && req.headers) || {};
    // The URL is built from the MATCHED ALLOWLIST ENTRY alone - never from
    // the header. "app.example.com:@evil.com" used to pass the allowlist (its
    // host part, cut at ':', matched) and then became the link itself,
    // sending reset and magic-link tokens to evil.com. Nor is the request's
    // port copied (audit 7): a client choosing it sent the tokens to whatever
    // listened on that port of the trusted host; an entry "host:port" pins one.
    const hp = requestHost(headers);
    if (hp && _state.trustedHosts) {
        for (let i = 0; i < _state.trustedHosts.length; i++) {
            const allowed = _state.trustedHosts[i];
            if (hp[0] === allowed || (hp[1] !== null && allowed === hp[0] + ":" + hp[1])) {
                return requestProto(headers, "https") + "://" + allowed;
            }
        }
    }
    if (hp && _state.trustRequestHost) {
        return requestProto(headers, "http") + "://" + hp[0]
            + (hp[1] !== null ? ":" + hp[1] : "");
    }
    // Round-11 LOW-10: one-shot warn on first null-return. See Lua
    // sibling. Mute after the first hit so a hostile scanner can't
    // flood the log.
    if (!_state.warnedHostMismatch) {
        _state.warnedHostMismatch = true;
        const list = _state.trustedHosts
            ? _state.trustedHosts.join(", ")
            : "(none configured)";
        const raw = String((_state.trustProxy && headers["x-forwarded-host"])
                           || headers.host || "(nil)")
            .slice(0, 200).replace(/[\x00-\x1f\x7f]/g, "?");
        log.warn("auth-flows: originFor refused host '" + raw
            + "'. trustedHosts = [" + list + "]. URL build "
            + "skipped; subsequent email sends will be silently "
            + "dropped until the host is added. Set publicOrigin "
            + "or trustRequestHost: true to override (behind a "
            + "proxy that sets X-Forwarded-Host, set trustProxy).");
    }
    return null;
}

// ── Cross-site guard (login CSRF) ──────────────────────────────────
// Every POST that signs a browser in or changes the account behind its
// session must come from the app's own pages: an attacker page that
// auto-submits a form to /login with the ATTACKER's credentials signs the
// victim in to the attacker's account. See the Lua sibling for the full
// reasoning. Sec-Fetch-Site must be same-origin or none (cross-site AND
// same-site refused); without it Origin - or Referer - must name the app
// (publicOrigin, a trustedHosts entry, or the request's own Host); with no
// provenance header at all only a JSON body is taken.

// Is the Content-Type JSON? Its essence, not a substring: "text/plain;
// x=application/json" is a CORS-safelisted type a cross-site form can send.
function isJsonCt(req) {
    const ct = req.headers && req.headers["content-type"];
    if (typeof ct !== "string") return false;
    return ct.split(";")[0].trim().toLowerCase() === "application/json";
}

// "scheme://authority" of an Origin / Referer value, lower-cased; null for
// anything else ("null", a missing or malformed header).
function headerOrigin(v) {
    if (typeof v !== "string") return null;
    const m = /^(https?:\/\/[^/?#]+)/.exec(v);
    return m ? m[1].toLowerCase() : null;
}

function originTrusted(o, headers) {
    if (_state.publicOrigin && o === headerOrigin(_state.publicOrigin)) return true;
    const authority = o.replace(/^https?:\/\//, "");
    if (_state.trustedHosts) {
        const hostOnly = authority.replace(/:\d+$/, "");
        for (const allowed of _state.trustedHosts) {
            const a = allowed.toLowerCase();
            if (authority === a || hostOnly === a) return true;
        }
    }
    const hp = requestHost(headers);
    return hp !== null
        && authority === (hp[0] + (hp[1] !== null ? ":" + hp[1] : "")).toLowerCase();
}

// `allowBare`: a request with no provenance header at all is let through
// whatever its body (logout, where forcing one only signs a user out).
function sameOriginRequest(req, allowBare) {
    const h = req.headers || {};
    const site = h["sec-fetch-site"];
    if (typeof site === "string" && site !== "") {
        return site === "same-origin" || site === "none";
    }
    if (h.origin !== undefined && h.origin !== null) {
        const o = headerOrigin(h.origin);
        return o !== null && originTrusted(o, h);
    }
    const r = headerOrigin(h.referer);
    if (r !== null) return originTrusted(r, h);
    if (allowBare) return true;
    if (!isJsonCt(req)) return false;
    try {
        const t = json.decode(req.body || "");
        return !!t && typeof t === "object";
    } catch (_) { return false; }
}

// Answer 403 and return true when the request is cross-site.
function refuseCrossSite(req, res, allowBare) {
    if (sameOriginRequest(req, allowBare)) return false;
    res.status(403).json({ error: "forbidden: cross-site request" });
    return true;
}

// ── Route handlers ─────────────────────────────────────────────────

function handleRegister(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    // 256 char upper bound prevents PBKDF2 amplification DoS - a
    // 10 MB submitted password would hash for multiple seconds at
    // the default 600k iters. 256 covers any realistic passphrase
    // (bcrypt's hard limit is 72 for comparison).
    if (!passwordLenOk(body.password)) {
        return res.status(400).json({ error: "invalid password length" });
    }
    // Pwned check runs BEFORE userFindByEmail so the same error
    // returns regardless of whether the email already exists. The
    // handler stays synchronous when the check is off, so the welcome
    // email is deferred past the response (see afterResponse).
    if (_state.checkPwnedPasswords) {
        return checkPwned(body.password).then((bad) => {
            if (bad) {
                return res.status(400).json({
                    error: "password appears in known data breaches; choose another",
                });
            }
            return registerAccount(req, res, body);
        });
    }
    return registerAccount(req, res, body);
}

function registerAccount(req, res, body) {
    // Hash FIRST, on both branches: PBKDF2 is by far the slowest step, and
    // running it only for new addresses let response time tell an attacker
    // which ones already have an account.
    const pwHash = crypto.hashPassword(body.password);
    const existing = findByEmail(body.email);
    if (existing) return genericOk(res);
    const uid = _state.userCreate(body.email, pwHash);
    const user = getUser(uid);
    if (!user) {
        return res.status(500).json({
            error: "user_create returned an id that user_get cannot resolve" });
    }

    const origin = originFor(req);
    afterResponse(() => {
        const token = issueToken(uid, ACTIONS.verify_email, _state.verifyTtl);
        if (origin) {
            const verifyUrl = origin + _state.prefix + "/verify?token=" + token;
            sendEmail(body.email, "welcome", { user, verify_url: verifyUrl, token });
        }
    });
    res.json({ ok: true });
}

// POST /auth/verify/resend { email } - enumeration-safe re-issue
// of the welcome / verify email for unverified users.
function handleVerifyResend(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    const user = findByEmail(body.email);
    if (!user || isVerified(user)) return genericOk(res);
    const uid = userId(user);
    const origin = originFor(req);
    afterResponse(() => {
        const token = issueToken(uid, ACTIONS.verify_email, _state.verifyTtl);
        if (origin) {
            const verifyUrl = origin + _state.prefix + "/verify?token=" + token;
            sendEmail(body.email, "welcome", { user, verify_url: verifyUrl, token });
        }
    });
    res.json({ ok: true });
}

// ── Email verification ─────────────────────────────────────────────
// Two steps; see the Lua sibling for the design. GET never consumes the
// token (mail scanners prefetch links): it renders a form or redirects to
// verifyFormRedirect. POST {token, password} keeps a password the mailbox
// holder also knows (wrong: 401, token still usable, counts toward the login
// lockout); POST {token, new_password} replaces it and everything a
// pre-registrant could have attached. Nothing is voided silently.

function wantsJson(req) {
    return isJsonCt(req);
}

// The token is a verified envelope (fixed alphabet) and the error strings
// are module constants, so nothing here needs escaping. No script; the
// token in the body is what a cross-site form cannot supply.
function defaultVerifyFormHtml(token, err) {
    const action = _state.prefix + "/verify";
    return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        + '<title>Verify your email</title></head>'
        + '<body style="font-family:sans-serif;max-width:400px;margin:4em auto;">'
        + '<h1>Verify your email</h1>'
        + (err ? ('<p role="alert"><strong>' + err + '</strong></p>') : '')
        + '<form method="POST" action="' + action + '">'
        + '<input type="hidden" name="token" value="' + token + '">'
        + '<p><label>Your password: <input type="password" name="password" '
        + 'autocomplete="current-password" required></label></p>'
        + '<button type="submit">Verify</button></form>'
        + '<h2>Did not choose a password, or forgot it?</h2>'
        + '<form method="POST" action="' + action + '">'
        + '<input type="hidden" name="token" value="' + token + '">'
        + '<p><label>New password: <input type="password" name="new_password" '
        + 'autocomplete="new-password" minlength="8" maxlength="256" required>'
        + '</label></p>'
        + '<button type="submit">Set password and verify</button></form>'
        + '<p style="color:#666;font-size:smaller">Setting a new password '
        + 'signs out every session of this account.</p></body></html>';
}

function verifyFail(req, res, status, msg) {
    if (wantsJson(req)) return res.status(status).json({ error: msg });
    return secureHtml(res).status(status).html(msg);
}

function verifyOk(req, res) {
    gcExpired();
    if (wantsJson(req)) return res.json({ ok: true, redirect: _state.verifyRedirect });
    return res.redirect(_state.verifyRedirect, 303);
}

function handleVerifyPage(req, res) {
    const token = req.query && req.query.token;
    const r = parseToken(token, ACTIONS.verify_email);
    if (!r[0]) {
        return secureHtml(res).status(400).html("verification failed: " + (r[1] || "?"));
    }
    if (tokenAlreadyUsed(token)) {
        return secureHtml(res).status(400).html("verification failed: replayed");
    }
    const user = getUser(r[0].sub);
    if (!user) return secureHtml(res).status(400).html("verification failed");
    if (isVerified(user)) return res.redirect(_state.verifyRedirect);
    if (_state.verifyFormRedirect) {
        const sep = _state.verifyFormRedirect.indexOf("?") >= 0 ? "&" : "?";
        return res.redirect(_state.verifyFormRedirect + sep + "token=" + token);
    }
    secureHtml(res).html(defaultVerifyFormHtml(token));
}

const VERIFY_WRONG_PASSWORD = "password does not match; to set a new password "
    + "instead, submit new_password";

async function handleVerify(req, res) {
    if (refuseCrossSite(req, res)) return;
    const body = parseBody(req);
    const token = body.token;
    const r = parseToken(token, ACTIONS.verify_email);
    if (!r[0]) return verifyFail(req, res, 400, "verification failed: " + (r[1] || "?"));
    const env = r[0];
    if (tokenAlreadyUsed(token)) {
        return verifyFail(req, res, 400, "verification failed: replayed");
    }
    const user = getUser(env.sub);
    if (!user) return verifyFail(req, res, 400, "verification failed");
    const uid = userId(user);
    if (isVerified(user)) {
        markTokenUsed(token, env.exp);
        return verifyOk(req, res);
    }

    if (body.new_password !== undefined) {
        const pw = body.new_password;
        if (!passwordLenOk(pw)) {
            return verifyFail(req, res, 400, "invalid password length");
        }
        if (await checkPwned(pw)) {
            return verifyFail(req, res, 400,
                "password appears in known data breaches; choose another");
        }
        const newHash = crypto.hashPassword(pw);
        if (!markTokenUsed(token, env.exp)) {
            return verifyFail(req, res, 400, "verification failed: replayed");
        }
        await replaceUnverifiedCredentials(req, res, user, uid, newHash);
        emitEvent(uid, "password_reset_completed", req, { metadata: { via: "verify" } });
        return verifyOk(req, res);
    }

    const pw = body.password;
    if (typeof pw !== "string") {
        return verifyFail(req, res, 400, "password or new_password required");
    }
    // The same lockout rows as /login (see the Lua sibling): no second,
    // unthrottled password oracle.
    const ipKey = attemptIpKey(uid, req);
    const locked = lockoutRemaining(ipKey) > 0 || lockoutRemaining(uid) > 0;
    const stored = !locked && passwordLenOk(pw, false) && storedPasswordHash(user);
    const ok = typeof stored === "string" && crypto.verifyPassword(pw, stored);
    if (!ok) {
        if (!locked) {
            bumpFailedLogin(ipKey, _state.maxFailedLogins);
            bumpFailedLogin(uid, _state.maxFailedLoginsPerAccount);
        }
        if (wantsJson(req)) {
            return res.status(401).json({ error: VERIFY_WRONG_PASSWORD,
                                          new_password_allowed: true });
        }
        return secureHtml(res).status(401).html(defaultVerifyFormHtml(token,
            "That password does not match. Try again, or set a new password below."));
    }
    if (!markTokenUsed(token, env.exp)) {
        return verifyFail(req, res, 400, "verification failed: replayed");
    }
    clearFailedLogins(ipKey);
    clearFailedLogins(uid);
    _state.userSetEmailVerified(uid, true);
    return verifyOk(req, res);
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

// JSON (login, a JSON magic-link POST), or for a browser (`asPage`: the
// magic-link page's form) an HTML form or redirect.
function startTotpPending(req, res, user, asPage) {
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.totp_pending,
                              _state.totpPendingTtl);
    if (!asPage) {
        return res.json({
            ok: true, pending_2fa: true, totp_token: token,
        });
    }
    if (_state.totpPendingRedirect) {
        const sep = _state.totpPendingRedirect.indexOf("?") >= 0 ? "&" : "?";
        return res.redirect(_state.totpPendingRedirect
                             + sep + "token=" + token);
    }
    secureHtml(res).html(defaultTotpFormHtml(token));
}

function handleLogin(req, res) {
    // Login CSRF: see "Cross-site guard".
    if (refuseCrossSite(req, res)) return;
    const body = parseBody(req);
    // 256 char upper bound matches register; prevents PBKDF2
    // amplification DoS via mega-passwords. Generic error keeps
    // enumeration-safety (over-length is just another wrong cred).
    if (!isEmailIsh(body.email)
        || !passwordLenOk(body.password, false)) {
        return res.status(400).json({ error: "invalid credentials" });
    }
    const user = findByEmail(body.email);
    // Lockout: when the user exists AND is currently locked, short-
    // circuit to the SAME 401 + "invalid credentials" the wrong-
    // password branch returns. Round-8 HIGH-4: prior code returned
    // 429 + Retry-After in this branch, leaking account existence
    // via the lockout signal (trip lockout against any candidate to
    // enumerate registered emails). The lockout still applies
    // internally - the counter ticks, the user still can't log in
    // until the window expires - but the wire response is now
    // indistinguishable from a wrong-password reply.
    const uid   = user ? userId(user) : null;
    const ipKey = user ? attemptIpKey(uid, req) : null;
    const preLocked = user
                      && (lockoutRemaining(ipKey) > 0
                          || lockoutRemaining(uid) > 0);
    // Timing-safe email enumeration defense. crypto.verifyPassword
    // (PBKDF2-SHA256, 600k iters by default) takes 50–200ms; a 401
    // that skipped the verify because the email was unknown would
    // return ~instantly, letting an attacker enumerate registered
    // emails by timing. Run verifyPassword unconditionally against a
    // pre-computed dummy hash on the unknown-email branch. See
    // init() for the dummy hash + the threat model. Opt-out with
    // enumerationSafe = false (test fixtures only).
    const pwHash = (user && user.password_hash) || _state._dummyPwhash;
    const pwOk   = (_state.enumerationSafe || Boolean(user))
                   && crypto.verifyPassword(body.password, pwHash);
    if (preLocked || !user || !user.password_hash || !pwOk) {
        if (user && !preLocked) {
            bumpFailedLogin(ipKey, _state.maxFailedLogins);
            bumpFailedLogin(uid, _state.maxFailedLoginsPerAccount);
        }
        return res.status(401).json({ error: "invalid credentials" });
    }
    if (_state.requireVerifiedEmail && !isVerified(user)) {
        return res.status(403).json({ error: "email not verified" });
    }
    clearFailedLogins(ipKey);
    clearFailedLogins(uid);
    if (_state.enableTotp) {
        const enrolled = totpEnrolled(user);
        if (enrolled === null)
            return res.status(500).json({ error: "auth-flows misconfigured" });
        if (enrolled) return startTotpPending(req, res, user);
    }
    return finishLogin(req, res, user, "password");
}

function handleLogout(req, res) {
    // user_id isn't known here without inspecting the session -
    // app's responsibility. Apps that want a "logout" event can
    // call auditLog.record inside their onLogout callback.
    // A cross-site POST (an attacker page auto-submitting a form) is refused,
    // as oauth's logout does: SameSite=Lax keeps the session cookie off it,
    // but the clearing Set-Cookie in the answer would still sign the victim
    // out. A client that sends no provenance header at all is let through:
    // forcing a logout is all a forged one could do. An async onLogout is
    // returned so the dispatcher awaits it.
    if (refuseCrossSite(req, res, true)) return;
    if (_state.onLogout) return _state.onLogout(req, res);
    res.redirect("/");
}

function handleMagicLink(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    let user = findByEmail(body.email);
    if (!user) {
        if (!_state.magicLinkAutoSignup) return genericOk(res);
        const uid = _state.userCreate(body.email, null);
        user = getUser(uid);
        // Guard the create->get race / adapter inconsistency: a nil user here
        // would mint a magic-link token with sub=null and then throw in
        // sendEmail(user...). Stay enumeration-safe (same shape as the
        // unknown-email path above) rather than 500.
        if (!user) return genericOk(res);
    }
    const origin = originFor(req);
    afterResponse(() => {
        const token = issueToken(userId(user), ACTIONS.magic_link,
            _state.magicLinkTtl, { eb: emailBinding(user) });
        if (origin) {
            const link = origin + _state.prefix + "/magic-link/consume?token=" + token;
            sendEmail(body.email, "magic_link", { user, link, token });
        }
    });
    res.json({ ok: true });
}

// ── Single-use links: GET shows, POST consumes ─────────────────────
// A mailed single-use link (magic link, email-change confirm and revoke) is
// not consumed by its GET: mail scanners prefetch links, and a prefetch
// signed the scanner in (the user's own click then answered "replayed"),
// confirmed a change nobody read, or cancelled it. The GET checks the token
// without using it and answers a page whose form POSTs it back; the POST
// consumes it - as the verify flow does. A JSON client POSTs {token} itself.

// The token is a verified envelope (fixed alphabet) and the strings are
// module constants, so nothing needs escaping. No script.
function linkFormHtml(path, token, title, button) {
    return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        + '<title>' + title + '</title></head>'
        + '<body style="font-family:sans-serif;max-width:400px;margin:4em auto;">'
        + '<h1>' + title + '</h1>'
        + '<form method="POST" action="' + _state.prefix + path + '">'
        + '<input type="hidden" name="token" value="' + token + '">'
        + '<button type="submit">' + button + '</button></form></body></html>';
}

// The GET side: the envelope and its single use, nothing consumed. Returns
// [token, env] - or null after answering 400.
function linkPageToken(req, res, action, fail) {
    const token = req.query && req.query.token;
    const r = parseToken(token, action);
    if (!r[0]) {
        secureHtml(res).status(400).html(fail + ": " + (r[1] || "?"));
        return null;
    }
    if (tokenAlreadyUsed(token)) {
        secureHtml(res).status(400).html(fail + ": replayed");
        return null;
    }
    return [token, r[0]];
}

function handleMagicLinkPage(req, res) {
    const t = linkPageToken(req, res, ACTIONS.magic_link, "magic link failed");
    if (!t) return;
    secureHtml(res).html(linkFormHtml("/magic-link/consume", t[0], "Sign in", "Sign in"));
}

function handleMagicLinkConsume(req, res) {
    // A cross-site form would sign the victim in to the attacker's account
    // with the attacker's own link (login CSRF).
    if (refuseCrossSite(req, res)) return;
    const result = consumeToken(parseBody(req).token, ACTIONS.magic_link);
    if (!result[0]) {
        return verifyFail(req, res, 400, "magic link failed: " + (result[1] || "?"));
    }
    const user = getUser(result[0].sub);
    // A magic link is bound to the address it was sent to: after an email
    // change, one still sitting in the old mailbox no longer signs in.
    if (!user || result[0].eb !== emailBinding(user))
        return verifyFail(req, res, 400, "magic link failed");
    // Magic-link clicks count as proof of email ownership. An unverified
    // account that HAS a password may carry one somebody else chose: the
    // click goes through the verify step (confirm it or set a new one, see
    // handleVerify) rather than signing in or silently replacing it. A
    // passwordless account (magicLinkAutoSignup) has nothing to keep.
    if (!isVerified(user)) {
        const uid = userId(user);
        if (storedPasswordHash(user) !== null) {
            const vtok = issueToken(uid, ACTIONS.verify_email, _state.verifyTtl);
            gcExpired();
            if (_state.verifyFormRedirect) {
                const sep = _state.verifyFormRedirect.indexOf("?") >= 0 ? "&" : "?";
                return res.redirect(_state.verifyFormRedirect + sep + "token=" + vtok);
            }
            return secureHtml(res).html(defaultVerifyFormHtml(vtok));
        }
        _state.userSetEmailVerified(uid, true);
        user.email_verified = true;
    }
    gcExpired();
    if (_state.enableTotp) {
        const enrolled = totpEnrolled(user);
        if (enrolled === null)
            return res.status(500).json({ error: "auth-flows misconfigured" });
        if (enrolled) return startTotpPending(req, res, user, !wantsJson(req));
    }
    return finishLogin(req, res, user, "magic_link");
}

// POST /auth/totp-verify { token, code } - second factor.
// Apps SHOULD rate-limit this route (e.g. ratelimit.middleware
// keyed on the body's token field) to bound brute-force on the
// 6-digit code space. The pending token is multi-use within TTL
// (lets users retry typos) and only burned on a successful code
// verify.
function handleTotpVerify(req, res) {
    if (!_state.enableTotp) {
        return res.status(404).json({ error: "totp not enabled" });
    }
    // Login CSRF with the attacker's own pending token and code.
    if (refuseCrossSite(req, res)) return;
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
    const user = getUser(env.sub);
    if (!user) return res.status(400).json({ error: "totp failed" });
    // Round-9 HIGH-4: pass `req` so totpVerify can gate per-IP too.
    const ok = _state.totpVerify(user, body.code, req);
    if (isThenable(ok)) {
        log.error("auth-flows: totpVerify returned a Promise; it must be synchronous");
        return res.status(500).json({ error: "auth-flows misconfigured" });
    }
    if (ok !== true) return res.status(401).json({ error: "invalid code" });
    // Round-8 MEDIUM-6: prior code discarded markTokenUsed's return,
    // so two concurrent verifies with the same {token, code} both
    // minted a session from one pending-2FA token. Act on the return
    // to bail when we lost the race; the OTHER request mints the
    // session.
    if (!markTokenUsed(body.token, env.exp)) {
        return res.status(400).json({ error: "totp token already used" });
    }
    gcExpired();
    return finishLogin(req, res, user, "password+totp");
}

function handlePasswordResetRequest(req, res) {
    const body = parseBody(req);
    if (!isEmailIsh(body.email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    const user = findByEmail(body.email);
    if (!user) return genericOk(res);
    const origin = originFor(req);
    afterResponse(() => {
        const token = issueToken(userId(user), ACTIONS.password_reset,
            _state.resetTtl, resetTokenExtra(user));
        if (origin) {
            const link = origin + _state.prefix
                + "/password-reset/confirm?token=" + token;
            sendEmail(body.email, "password_reset", { user, link, token });
        }
    });
    res.json({ ok: true });
}

async function handlePasswordResetConfirm(req, res) {
    if (refuseCrossSite(req, res)) return;
    const body = parseBody(req);
    // Same upper bound as handleRegister; see comment there.
    if (!passwordLenOk(body.password)) {
        return res.status(400).json({ error: "invalid password length" });
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
    const user = getUser(result[0].sub);
    if (!user || !resetBindingHolds(result[0], user))
        return res.status(400).json({ error: "reset failed" });
    const newHash = crypto.hashPassword(body.password);
    if (!isVerified(user)) {
        // The reset link proves the mailbox and its holder chose this
        // password: verified, as a verify with new_password - see Lua.
        await replaceUnverifiedCredentials(req, res, user, result[0].sub, newHash);
        emitEvent(result[0].sub, "password_reset_completed", req);
        gcExpired();
        return res.json({ ok: true });
    }
    _state.userSetPassword(result[0].sub, newHash);
    dropPendingEmailChange(result[0].sub);
    // A successful reset demonstrates email control; clear any
    // outstanding lockout so the new password works immediately.
    clearAllFailedLogins(result[0].sub);
    // Audit + app-side session revocation. Recommended onPasswordReset
    // body: `(req, res, user) => session.destroyAll(user.id)`.
    emitEvent(result[0].sub, "password_reset_completed", req);
    await runOnPasswordReset(req, res, user);
    gcExpired();
    res.json({ ok: true });
}

function handleEmailChange(req, res) {
    const uid = req.ctx && req.ctx.user_id;
    if (!uid) return res.status(401).json({ error: "not authenticated" });
    if (refuseCrossSite(req, res)) return;
    const body = parseBody(req);
    if (!isEmailIsh(body.new_email)) {
        return res.status(400).json({ error: "invalid email" });
    }
    // A session is not enough: whoever stole one would move the account to
    // an address they read and own it for good through a reset there. The
    // current password (counted toward the login lockout) proves the account
    // holder; emailChangeReauth(req, user) -> true replaces it for an app with
    // its own proof of a recent sign-in, or passwordless accounts.
    const current = getUser(uid);
    if (!current) return res.status(401).json({ error: "not authenticated" });
    let reauthed = false;
    if (_state.emailChangeReauth) {
        try { reauthed = _state.emailChangeReauth(req, current) === true; }
        catch (_) { reauthed = false; }
    }
    if (!reauthed) {
        const pw = body.password;
        if (typeof pw !== "string" || pw === "") {
            return res.status(401).json({ error: "current password required" });
        }
        // The lockout rows /login uses: keyed by the account's own id.
        const acct = userId(current) ?? uid;
        const ipKey = attemptIpKey(acct, req);
        const locked = lockoutRemaining(ipKey) > 0 || lockoutRemaining(acct) > 0;
        const stored = !locked && passwordLenOk(pw, false) && storedPasswordHash(current);
        if (!(typeof stored === "string" && crypto.verifyPassword(pw, stored))) {
            if (!locked) {
                bumpFailedLogin(ipKey, _state.maxFailedLogins);
                bumpFailedLogin(acct, _state.maxFailedLoginsPerAccount);
            }
            return res.status(401).json({ error: "invalid credentials" });
        }
    }
    if (findByEmail(body.new_email)) {
        return res.status(409).json({ error: "email already in use" });
    }

    // Round-10 HIGH-1: compute origin FIRST. Pre-fix the upsert
    // ran before the origin check, so a hostile Host header could
    // clobber the victim's pending email-change row even though
    // no mail was sent. Bailing here keeps the DB untouched while
    // preserving the {ok:true} enumeration-safe response.
    const origin = originFor(req);
    if (!origin) return res.json({ ok: true });

    // Round-11 MEDIUM-9: reject when a pending row already exists.
    // See Lua sibling for the threat model - concurrent submit
    // silently destroyed the prior pending change.
    // A confirmed change keeps its row until its revoke link expires, and
    // blocks a new one meanwhile: a thief must not bury it under a second.
    const existing = db.query(
        "SELECT new_email, confirmed_at FROM _hull_auth_pending_email_changes "
        + "WHERE user_id = ? AND expires_at > ? LIMIT 1",
        [uid, time.now()]);
    if (existing && existing.length > 0) {
        if (existing[0].confirmed_at !== null && existing[0].confirmed_at !== undefined) {
            return res.status(409).json({
                error: "a recent email change can still be revoked; try again later",
            });
        }
        return res.status(409).json({
            error: "pending email change exists",
            new_email: existing[0].new_email,
        });
    }

    const now = time.now();
    const token = issueToken(uid, ACTIONS.email_change,
        _state.emailChangeTtl, { new_email: body.new_email });
    const tokenHash = encoding.hex.encode(crypto.sha256(token));
    // An expired row (not reaped yet) is replaced whole: an upsert kept its
    // old_email / confirmed_at.
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?", [uid]);
    db.exec("INSERT INTO _hull_auth_pending_email_changes "
            + "(user_id, new_email, token_hash, created_at, expires_at) "
            + "VALUES (?, ?, ?, ?, ?)",
            [uid, body.new_email, tokenHash, now, now + _state.emailChangeTtl]);

    const user = current;
    const link = origin + _state.prefix
        + "/email-change/confirm?token=" + token;
    sendEmail(body.new_email, "email_change", {
        user, link, token, new_email: body.new_email,
    });
    // Defense in depth: notify the OLD address with a revoke link
    // if templates.email_change_notify is provided. Guard `user` (nil only on
    // a pathological row-deleted-mid-request race): the confirm email above
    // passes it through a nil-safe template ctx, but user.email below is a
    // hard deref.
    if (user && _state.templates.email_change_notify) {
        // Bound to THIS change (`ch`, its confirm token's hash): a revoke link
        // from an earlier change does not cancel or undo a later one.
        const revokeTok = issueToken(uid, ACTIONS.email_change_revoke,
            _state.emailChangeTtl, { ch: tokenHash });
        const revokeUrl = origin + _state.prefix
            + "/email-change/revoke?token=" + revokeTok;
        sendEmail(user.email, "email_change_notify", {
            user, revoke_url: revokeUrl, revoke_token: revokeTok,
            new_email: body.new_email,
        });
    }
    res.json({ ok: true });
}

// GET /auth/email-change/revoke?token=... - the page; POST consumes it (see
// "Single-use links"). The old-address holder undoes the email change the
// link was sent for: a pending one is cancelled (its row deleted, so its
// confirm link stops working); one already confirmed is reversed - the old
// address restored - for as long as the link lives (emailChangeTtl from the
// request). Either way every session of the account is revoked through
// onPasswordReset: the change may have come from a stolen one.
function handleEmailChangeRevokePage(req, res) {
    const t = linkPageToken(req, res, ACTIONS.email_change_revoke, "revoke failed");
    if (!t) return;
    secureHtml(res).html(linkFormHtml("/email-change/revoke", t[0],
        "Undo the email change", "Undo the change"));
}

async function handleEmailChangeRevoke(req, res) {
    if (refuseCrossSite(req, res)) return;
    const result = consumeToken(parseBody(req).token, ACTIONS.email_change_revoke);
    if (!result[0]) {
        return verifyFail(req, res, 400, "revoke failed: " + (result[1] || "?"));
    }
    const env = result[0];
    const rows = db.query(
        "SELECT token_hash, old_email, confirmed_at FROM _hull_auth_pending_email_changes "
        + "WHERE user_id = ?", [env.sub]);
    const row = rows && rows[0];
    if (!row || typeof env.ch !== "string" || typeof row.token_hash !== "string"
        || !crypto.constantTimeEq(row.token_hash, env.ch)) {
        return verifyFail(req, res, 400, "revoke failed");
    }
    const user = getUser(env.sub);
    if (!user) return verifyFail(req, res, 400, "revoke failed");
    let restored = false;
    if (row.confirmed_at !== null && row.confirmed_at !== undefined) {
        const old = row.old_email;
        const holder = typeof old === "string" ? findByEmail(old) : null;
        if (typeof old !== "string"
            || (holder && String(userId(holder)) !== String(env.sub))) {
            // Taken since by another account: nothing to restore to.
            log.warn("auth-flows: email change of account " + String(env.sub)
                + " cannot be reverted: its previous address is in use; sessions revoked");
            await runOnPasswordReset(req, res, user);
            return verifyFail(req, res, 409, "revoke failed: the previous address is in use");
        }
        _state.userSetEmail(env.sub, old);
        // The revoke link reached the old mailbox: it is proven again.
        _state.userSetEmailVerified(env.sub, true);
        user.email = old;
        restored = true;
    }
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?", [env.sub]);
    await runOnPasswordReset(req, res, user);
    emitEvent(env.sub, "email_change_revoked", req,
              { metadata: { by: "old_address", restored } });
    gcExpired();
    if (wantsJson(req)) return res.json({ ok: true, restored });
    secureHtml(res).html(restored
        ? "Email change undone: your previous address is restored."
        : "Email change canceled.");
}

// Does the pending change still match this confirm link? There must be a
// pending (unconfirmed) row, for the envelope's new_email, and its token_hash
// must be THIS token's: only the latest link of the pending change confirms
// it, and none once the row is gone (revoked, superseded) or confirmed.
function pendingChangeMatches(env, token) {
    const rows = db.query(
        "SELECT new_email, token_hash, confirmed_at FROM _hull_auth_pending_email_changes "
        + "WHERE user_id = ?", [env.sub]);
    const th = encoding.hex.encode(crypto.sha256(token));
    return !!rows && rows.length > 0
        && (rows[0].confirmed_at === null || rows[0].confirmed_at === undefined)
        && rows[0].new_email === env.new_email
        && typeof rows[0].token_hash === "string"
        && crypto.constantTimeEq(rows[0].token_hash, th);
}

function handleEmailChangePage(req, res) {
    const t = linkPageToken(req, res, ACTIONS.email_change, "email change failed");
    if (!t) return;
    if (!pendingChangeMatches(t[1], t[0])) {
        return secureHtml(res).status(400).html("email change failed");
    }
    secureHtml(res).html(linkFormHtml("/email-change/confirm", t[0],
        "Confirm your new email address", "Confirm"));
}

function handleEmailChangeConfirm(req, res) {
    if (refuseCrossSite(req, res)) return;
    const token = parseBody(req).token;
    const result = consumeToken(token, ACTIONS.email_change);
    if (!result[0]) {
        return verifyFail(req, res, 400, "email change failed: " + (result[1] || "?"));
    }
    const env = result[0];
    const user = getUser(env.sub);
    if (!user || !pendingChangeMatches(env, token)) {
        return verifyFail(req, res, 400, "email change failed");
    }
    const oldEmail = user.email;
    _state.userSetEmail(env.sub, env.new_email);
    _state.userSetEmailVerified(env.sub, true);
    // With a revoke link out (email_change_notify), the row stays, confirmed
    // and holding the old address, until that link expires: the old address
    // can still undo the change. Without one there is nothing to undo it with.
    if (_state.templates.email_change_notify) {
        db.exec("UPDATE _hull_auth_pending_email_changes "
                + "SET confirmed_at = ?, old_email = ? WHERE user_id = ?",
                [time.now(), oldEmail, env.sub]);
    } else {
        db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
                [env.sub]);
    }
    emitEvent(env.sub, "email_changed", req,
              { metadata: { old_email: oldEmail, new_email: env.new_email } });
    return verifyOk(req, res);
}

function registerRoutes(app) {
    const p = _state.prefix;
    // Opt-in per-IP rate limit on /login + /password-reset/request +
    // /magic-link. Installed BEFORE the handler so abusive traffic is
    // rejected before any DB or PBKDF2 work. See loginRatelimit opt
    // for the threat model. Per-IP key derived from x-forwarded-for
    // then req.remote_addr; falls back to "_anon" so a malformed
    // request can't bypass the bucket entirely.
    if (_state.loginRatelimit) {
        const rlOpts = typeof _state.loginRatelimit === "object"
            ? _state.loginRatelimit : {};
        // Per-IP key via the shared hull:web:_request helper
        // (trustProxy -> the last XFF entry, the peer our proxy saw
        // -> remote_addr). The client writes every entry to the left
        // of it, so keying on those (or the whole chain) let a client
        // mint a new bucket per request. App-supplied opts.key still wins.
        const mw = ratelimit.middleware({
            limit:  rlOpts.limit  || 20,
            window: rlOpts.window || 300,
            key:    rlOpts.key || ((req) =>
                _request.limitKey(_request.clientIp(req, _state.trustProxy)) || "_anon"),
        });
        app.use("POST", p + "/register", mw);
        app.use("POST", p + "/login", mw);
        app.use("POST", p + "/magic-link", mw);
        app.use("POST", p + "/password-reset/request", mw);
    }

    app.post(p + "/register",                 handleRegister);
    app.get (p + "/verify",                   handleVerifyPage);
    app.post(p + "/verify",                   handleVerify);
    app.post(p + "/verify/resend",            handleVerifyResend);
    app.post(p + "/login",                    handleLogin);
    app.post(p + "/logout",                   handleLogout);
    app.post(p + "/magic-link",               handleMagicLink);
    app.get (p + "/magic-link/consume",       handleMagicLinkPage);
    app.post(p + "/magic-link/consume",       handleMagicLinkConsume);
    app.post(p + "/password-reset/request",   handlePasswordResetRequest);
    app.post(p + "/password-reset/confirm",   handlePasswordResetConfirm);
    app.post(p + "/email-change",             handleEmailChange);
    app.get (p + "/email-change/confirm",     handleEmailChangePage);
    app.post(p + "/email-change/confirm",     handleEmailChangeConfirm);
    app.get (p + "/email-change/revoke",      handleEmailChangeRevokePage);
    app.post(p + "/email-change/revoke",      handleEmailChangeRevoke);
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
 * DB-backend-agnostic - issues standard INSERT / UPDATE / SELECT
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
    // Quote the app-supplied table name for the connection's dialect so a
    // reserved word ("user", "order") or a future MySQL backend (backtick) is
    // safe. The default connection's backend is known by the time this runs.
    const name = opts.table || "users";
    // The adapter runs on the internal connection with stdlib identity, which
    // the _hull_* namespace guard lets through: refuse the reserved names.
    if (typeof name !== "string" || name === "" || /^_hull_/i.test(name)) {
        throw new Error("auth-flows.standardUsers: table must be a non-empty "
            + "name outside the reserved _hull_ namespace");
    }
    const tbl = db.quoteIdentifier(name);
    const idGen = opts.idGen || (() => crypto.randomToken(16, "hex"));

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
    // Canonical `secret`; back-compat alias `stateSecret` (same HMAC key).
    const secret = opts.secret || opts.stateSecret;
    if (typeof secret !== "string" || secret.length < 32) {
        throw new Error("auth-flows.init: secret must be a string >= 32 bytes");
    }
    if (typeof opts.emailSend !== "function") {
        throw new Error("auth-flows.init: emailSend(to, subject, html, text) required");
    }
    if (!opts.templates || typeof opts.templates !== "object") {
        throw new Error("auth-flows.init: templates object required");
    }
    // Unverified accounts can sign in, so a pre-registrant (anyone can
    // register any address) can hold a session when the mailbox holder sets
    // the password at verification. Sessions are the app's: onPasswordReset
    // is the only place they can be revoked, so it is required here.
    // (Checked before any state changes, so a refused init leaves none.)
    const rve = opts.requireVerifiedEmail !== undefined
        ? opts.requireVerifiedEmail : _state.requireVerifiedEmail;
    if (!rve && typeof opts.onPasswordReset !== "function") {
        throw new Error("auth-flows.init: requireVerifiedEmail: false needs "
            + "onPasswordReset (e.g. (req, res, user) => session.destroyAll(user.id)): "
            + "it is what revokes a pre-registrant's sessions when the address "
            + "owner sets the password; pass a no-op function if the app keeps "
            + "no sessions");
    }
    // Round-9 HIGH-1: require ONE of publicOrigin / trustedHosts.
    // See the Lua sibling docstring for the threat model.
    const hasOrigin = typeof opts.publicOrigin === "string"
                      && opts.publicOrigin.length > 0;
    const hasHosts = Array.isArray(opts.trustedHosts)
                     && opts.trustedHosts.length > 0;
    const trustReq = opts.trustRequestHost === true;
    if (!hasOrigin && !hasHosts && !trustReq) {
        throw new Error("auth-flows.init: pass `publicOrigin: "
            + "\"https://app.example.com\"` OR `trustedHosts: "
            + "[\"app.example.com\", ...]` OR `trustRequestHost: true` "
            + "(dev/test only). Click-through URLs are built from "
            + "this; without it, req.headers.host is attacker-"
            + "controlled and a hostile Host header reroutes the "
            + "link to a phishing origin.");
    }
    if (trustReq && !(hasOrigin || hasHosts)) {
        log.warn("auth-flows: trustRequestHost = true - falling back to "
            + "req.headers.host for URL construction. Vulnerable to "
            + "host-header injection; use publicOrigin / trustedHosts "
            + "in production.");
    }
    if (hasOrigin) {
        if (!/^https?:\/\//.test(opts.publicOrigin)) {
            throw new Error("auth-flows.init: publicOrigin must start "
                + "with http:// or https://");
        }
        if (opts.publicOrigin.endsWith("/")) {
            opts.publicOrigin = opts.publicOrigin.slice(0, -1);
        }
    }
    if (hasHosts) {
        // Each entry is a host or "host:port", the shapes originFor can
        // match (see the Lua sibling).
        for (let i = 0; i < opts.trustedHosts.length; i++) {
            const h = opts.trustedHosts[i];
            if (typeof h !== "string" || h === "") {
                throw new Error("auth-flows.init: trustedHosts entries "
                    + "must be non-empty strings (got " + typeof h + ")");
            }
            const m = /^(?:\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+)(?::(\d{1,5}))?$/.exec(h);
            if (!m || (m[1] !== undefined && Number(m[1]) > 65535)) {
                throw new Error("auth-flows.init: trustedHosts entry '"
                    + h + "' must be a host name or \"host:port\" (IPv6 "
                    + "literals bracketed, e.g. \"[::1]\"); emailed links "
                    + "carry only the port an entry names.");
            }
        }
    }
    const requiredUser = [
        "userFindByEmail", "userGet", "userCreate",
        "userSetPassword", "userSetEmail", "userSetEmailVerified",
    ];
    // opts.users (typically from authFlows.standardUsers(...))
    // bulk-fills the 6 callbacks; explicit opts.userX still wins.
    // Adapter keys are short ("findByEmail", "create", etc.) so a
    // plain `users.create` reads naturally at the call site.
    //
    // Derive the short key from the long key by stripping the "user"
    // prefix and lower-casing the next char (camelCase). Mirrors
    // Lua's `k:sub(6)` prefix-strip so adding a new callback only
    // needs editing requiredUser[] above - the adapter wiring picks
    // it up automatically.
    if (opts.users && typeof opts.users === "object") {
        for (let i = 0; i < requiredUser.length; i++) {
            const k = requiredUser[i];
            const short = k.charAt(4).toLowerCase() + k.substring(5);
            if (opts[k] === undefined
                && typeof opts.users[short] === "function") {
                opts[k] = opts.users[short];
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

    // A byte string, like the Lua side: bytes.toU8 keeps its bytes.
    _state.stateSecret = encoding.bytes.toU8(secret);
    _state.emailSend      = opts.emailSend;
    _state.publicOrigin   = opts.publicOrigin || null;
    _state.trustedHosts   = opts.trustedHosts || null;
    _state.trustRequestHost = opts.trustRequestHost === true;
    // Honor X-Forwarded-Proto only behind a trusted proxy. Off by default so a
    // spoofed header can't downgrade an emailed https link to http on a
    // directly-exposed app; the per-branch scheme default (https for an
    // allowlisted host, http for the trustRequestHost dev path) is used
    // otherwise.
    _state.trustProxy = opts.trustProxy === true;
    // Round-12 MEDIUM-1: reset the one-shot host-mismatch warn so a
    // hot-reload that fixes / changes the allowlist gets a fresh
    // diagnostic on the next bad host. See Lua sibling.
    _state.warnedHostMismatch = false;
    if (opts.userSanitize !== undefined
        && typeof opts.userSanitize !== "function") {
        throw new Error("auth-flows.init: userSanitize must be a "
            + "function (user) -> safeUser");
    }
    _state.userSanitize   = opts.userSanitize || null;
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
    _state.maxFailedLoginsPerAccount = opts.maxFailedLoginsPerAccount
                                  || _state.maxFailedLoginsPerAccount;
    _state.lockoutDuration      = opts.lockoutDuration
                                  || _state.lockoutDuration;
    _state.checkPwnedPasswords  = opts.checkPwnedPasswords === true;
    _state.pwnedEndpoint        = opts.pwnedEndpoint || null;
    _state.signInLog            = opts.signInLog === true;
    _state.onPasswordReset      = opts.onPasswordReset || null;
    _state.loginRatelimit       = opts.loginRatelimit !== undefined
                                  ? opts.loginRatelimit : false;
    // emailRateLimit: false disables, a {limit,window} object overrides,
    // undefined keeps the default. Reset the per-process sliding-window
    // state so re-init in tests starts with a clean bucket pool.
    if (opts.emailRateLimit !== undefined) {
        _state.emailRateLimit = opts.emailRateLimit;
    }
    _emailRl = new Map();

    // Timing-safe email enumeration defense - see handleLogin. The
    // dummy hash is computed once per process and reused per request
    // AND per re-init (test fixtures call init() many times; each
    // PBKDF2 was costing 50-200ms of boot time before this cache).
    if (!_state._dummyPwhash) {
        _state._dummyPwhash = crypto.hashPassword(
            "auth-flows-dummy-sentinel-never-matches-real-password");
    }
    _state.verifyTtl       = opts.verifyTtl       || _state.verifyTtl;
    _state.resetTtl        = opts.resetTtl        || _state.resetTtl;
    _state.magicLinkTtl    = opts.magicLinkTtl    || _state.magicLinkTtl;
    _state.emailChangeTtl  = opts.emailChangeTtl  || _state.emailChangeTtl;
    _state.prefix          = opts.prefix          || _state.prefix;
    _state.verifyRedirect  = opts.verifyRedirect  || _state.verifyRedirect;
    if (opts.verifyFormRedirect !== undefined && opts.verifyFormRedirect !== null
        && typeof opts.verifyFormRedirect !== "string") {
        throw new Error("auth-flows.init: verifyFormRedirect must be a path string");
    }
    _state.verifyFormRedirect = opts.verifyFormRedirect || null;
    if (opts.totpDisable !== undefined && opts.totpDisable !== null
        && typeof opts.totpDisable !== "function") {
        throw new Error("auth-flows.init: totpDisable must be a function(userId)");
    }
    _state.totpDisable = opts.totpDisable || null;
    if (opts.emailChangeReauth !== undefined && opts.emailChangeReauth !== null
        && typeof opts.emailChangeReauth !== "function") {
        throw new Error("auth-flows.init: emailChangeReauth must be a function(req, user)");
    }
    _state.emailChangeReauth = opts.emailChangeReauth || null;
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
    // A table made before a confirmed email change was kept (for revoke):
    // add the columns that keep it. A failed ALTER (another instance added the
    // column first) is re-checked, as session.js does.
    const cols = db.tableColumns("_hull_auth_pending_email_changes") || [];
    for (const [name, ddl] of [["old_email", "old_email TEXT"],
                               ["confirmed_at", "confirmed_at INTEGER"]]) {
        if (cols.includes(name)) continue;
        try {
            db.exec("ALTER TABLE _hull_auth_pending_email_changes ADD COLUMN " + ddl);
        } catch (e) {
            if (!(db.tableColumns("_hull_auth_pending_email_changes") || []).includes(name)) throw e;
        }
    }

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
    const user = findByEmail(email);
    if (!user) return;
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.password_reset, _state.resetTtl,
        resetTokenExtra(user));
    const link = (resetUrlPrefix || "") + _state.prefix
        + "/password-reset/confirm?token=" + token;
    sendEmail(email, "password_reset", { user, link, token });
}

function sendMagicLink(email, magicUrlPrefix) {
    if (!_state.initialized) throw new Error("auth-flows: call init() first");
    if (!isEmailIsh(email)) throw new Error("auth-flows.sendMagicLink: invalid email");
    let user = findByEmail(email);
    if (!user) {
        if (!_state.magicLinkAutoSignup) return;
        const uid = _state.userCreate(email, null);
        user = getUser(uid);
        // create->get race guard (see handleMagicLink): a nil user would mint
        // a sub=null token and throw in sendEmail.
        if (!user) return;
    }
    const uid = userId(user);
    const token = issueToken(uid, ACTIONS.magic_link, _state.magicLinkTtl,
        { eb: emailBinding(user) });
    const link = (magicUrlPrefix || "") + _state.prefix
        + "/magic-link/consume?token=" + token;
    sendEmail(email, "magic_link", { user, link, token });
}

const _test = {
    originFor,
    state: _state,
    issueToken,
    consumeToken,
    parseToken,
    markTokenUsed,
    tokenAlreadyUsed,
    renderTemplate,
    gcExpired,
    isEmailIsh,
    parseBody,
    sameOriginRequest,
    ACTIONS,
    emailRateAllow: (to) => emailRateAllow(to),
    emailRateReset: () => { _emailRl = new Map(); },
    reset: () => {
        _state.stateSecret = null;
        _state.emailSend      = null;
        _state.publicOrigin   = null;
        _state.trustedHosts   = null;
        _state.trustRequestHost = false;
        _state.userSanitize   = null;
        _state.warnedHostMismatch = false;
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
        _state.verifyFormRedirect   = null;
        _state.totpDisable          = null;
        _state.emailChangeReauth    = null;
        _state.checkPwnedPasswords  = false;
        _state.pwnedEndpoint        = null;
        _state.maxFailedLogins      = 5;
        _state.maxFailedLoginsPerAccount = 50;
        _state.lockoutDuration      = 15 * 60;
        _state.signInLog            = false;
        _state.onPasswordReset      = null;
        _state.loginRatelimit       = false;
        _state.initialized          = false;
    },
};

const authFlows = {
    init, routes, standardUsers,
    sendVerifyEmail, sendPasswordReset, sendMagicLink,
    _test,
};
export { authFlows };
