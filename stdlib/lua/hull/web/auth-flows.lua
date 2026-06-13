--- Transactional auth-flow recipes: registration / email-verify /
--- login / password-reset / magic-link / email-change.
--
-- @module hull.web.auth-flows
-- @license AGPL-3.0-or-later
--
-- ## What this module provides
--
-- A bundle of mount-able routes that cover the universal-need-list
-- of credential-managed apps:
--
--   POST  /auth/register                 -- email + password signup
--   GET   /auth/verify?token=...         -- click-to-verify email
--   POST  /auth/login                    -- email + password
--   POST  /auth/logout                   -- clears app's session
--   POST  /auth/magic-link               -- request a one-tap login
--   GET   /auth/magic-link/consume?...   -- click-through login
--   POST  /auth/password-reset/request   -- forgot-password
--   POST  /auth/password-reset/confirm   -- new password
--   POST  /auth/email-change             -- request a change
--   GET   /auth/email-change/confirm     -- click on new addr to swap
--
-- ## What this module does NOT do
--
--   * Own the users table. The app provides `user_*` callbacks
--     (find_by_email, get, create, set_password, set_email,
--     set_email_verified) so existing user-model migrations and
--     custom shapes drop in without re-platforming. Greenfield
--     apps can use the example schema in
--     `examples/auth_flows/migrations/`.
--   * Own sessions. After a successful login (any flow), the
--     module calls `on_login(req, res, user)`; the app uses that
--     to issue a session cookie / mark `req.ctx.session`. Same for
--     `on_logout(req, res)`.
--   * Render email templates. The app supplies render functions
--     that return `{ subject, html, text }`. Module errors at
--     send-time if a required template is missing.
--   * 2FA. The app composes `hull/web/middleware/totp` separately;
--     `on_login` is where `req.ctx.session.pending_2fa = true`
--     belongs if the app has enrolled the user. See
--     `examples/auth_flows/app.lua` for a worked example.
--
-- ## Security
--
--   * All click-through tokens are HMAC-signed, stateless envelopes:
--     `base64url(payload) || "." || hex(HMAC-SHA256(state_secret, body))`.
--     Payload is `{user_id, action, exp, nonce}`. Verify is
--     constant-time via crypto.hmac_sha256_verify.
--   * Single-use enforced via `_hull_auth_used_tokens` (hash of the
--     token bytes + used_at + expires_at). Replay fails the second
--     check.
--   * Password hashing via `crypto.hash_password` (PBKDF2-SHA256 at
--     the C layer). Verify via `crypto.verify_password`
--     (constant-time).
--   * Magic-link TTL defaults to 10 minutes (shorter than verify /
--     reset to limit attack window for high-value auth).
--   * Email-enumeration mitigation: register / password-reset /
--     magic-link / email-change all return generic success/error
--     shapes that don't reveal whether the email exists. The actual
--     email is only sent if the address is registered. (Standard
--     practice; can be disabled via `enumeration_safe = false` for
--     apps where convenience trumps the leak.)
--   * Re-verify-on-email-change: changing email puts the new
--     address in `_hull_auth_pending_email_changes`; the row only
--     swaps onto the user record after the user clicks the link
--     sent to the new address. Old email stays active until then.
--
-- ## Usage
--
--     local authflows = require("hull.web.auth-flows")
--     authflows.init({
--         state_secret  = env.get("AUTH_FLOWS_SECRET"),
--         email_send    = function(to, subject, html, text) ... end,
--         templates     = {
--             welcome        = function(ctx) ... end,
--             verify         = function(ctx) ... end,
--             magic_link     = function(ctx) ... end,
--             password_reset = function(ctx) ... end,
--             email_change   = function(ctx) ... end,
--         },
--         user_find_by_email     = function(email) ... end,
--         user_get               = function(user_id) ... end,
--         user_create            = function(email, password_hash) ... end,
--         user_set_password      = function(user_id, password_hash) ... end,
--         user_set_email         = function(user_id, email) ... end,
--         user_set_email_verified = function(user_id, verified) ... end,
--         on_login  = function(req, res, user) ... end,
--         on_logout = function(req, res) ... end,
--     })
--     authflows.routes(app)

local crypto   = require("hull.crypto")
local envelope = require("hull.crypto.envelope")
local db     = require("hull.db")
local time   = require("hull.time")
local json   = require("hull.json")

local M = {}

-- ── Module state ───────────────────────────────────────────────────

local _state = {
    state_secret_hex      = nil,
    -- TTLs in seconds. Verify and reset can be long; magic-link short.
    verify_ttl            = 86400,   -- 24h
    reset_ttl             = 3600,    -- 1h
    magic_link_ttl        = 600,     -- 10min
    email_change_ttl      = 86400,   -- 24h
    -- Mount prefix. Defaults to "/auth"; apps in mixed-routing setups
    -- can change this.
    prefix                = "/auth",
    enumeration_safe      = true,
    -- Magic-link to an unknown email: silently no-op by default
    -- (enumeration-safe; matches the rest of the unknown-email
    -- responses). Opt-in to auto-create a passwordless user with
    -- `magic_link_auto_signup = true`.
    magic_link_auto_signup = false,
    -- Block login until the user's email has been verified by
    -- default (Stripe-style). Opt-out with
    -- `require_verified_email = false` to allow login but pass
    -- `email_verified = false` on the user object so the app's
    -- routes can decide what's gated.
    require_verified_email = true,
    email_send            = nil,
    templates             = {},
    -- User-storage hooks. All REQUIRED at init time; init() errors
    -- with a list of missing ones for fast feedback.
    user_find_by_email      = nil,
    user_get                = nil,
    user_create             = nil,
    user_set_password       = nil,
    user_set_email          = nil,
    user_set_email_verified = nil,
    -- Session hooks.
    on_login              = nil,
    on_logout             = nil,
    -- TOTP composition. Default off; opt in by setting enable_totp
    -- and providing user_totp_enrolled + totp_verify. With it on,
    -- a successful password login OR magic-link click for an
    -- enrolled user does NOT immediately invoke on_login. Instead
    -- the module issues a short-TTL "pending 2FA" token and waits
    -- for a follow-up POST /auth/totp-verify before resuming the
    -- normal on_login(req, res, user) handoff.
    enable_totp           = false,
    user_totp_enrolled    = nil,
    totp_verify           = nil,
    totp_pending_ttl      = 300,   -- 5 minutes; tight enough that
                                   -- a stolen pending cookie can't
                                   -- be brute-forced in practice
                                   -- (apps SHOULD also rate-limit
                                   -- /totp-verify; see the route
                                   -- docstring).
    totp_pending_redirect = nil,   -- nil = render default HTML form
                                   -- on magic-link consume; set to
                                   -- a path to redirect there with
                                   -- ?token=... appended for a
                                   -- custom 2FA UI.
    -- Hardening: account lockout.
    -- After max_failed_logins consecutive wrong-password attempts
    -- the user's row in _hull_auth_login_attempts trips a
    -- locked_until window. handle_login short-circuits with
    -- 429 + Retry-After during that window. Counter clears on
    -- successful login or password-reset confirm.
    max_failed_logins     = 5,
    lockout_duration      = 15 * 60,   -- 15 min

    -- Hardening: pwned-password check (opt-in). When true, register
    -- and password-reset confirm reject passwords that appear in
    -- HIBP's breach corpus (k-anonymity range API; the password
    -- itself never leaves the host). Apps MUST add
    -- api.pwnedpasswords.com to manifest.hosts. Fail-open on HIBP
    -- outage. See hull/web/pwned.
    check_pwned_passwords = false,
    -- Override endpoint (tests pass localhost mock here).
    pwned_endpoint        = nil,

    -- Optional post-action redirects.
    verify_redirect       = "/",
    login_redirect        = "/",
    _initialized          = false,
}

-- ── Schema ─────────────────────────────────────────────────────────

local SCHEMA = [[
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
]]

-- ── Private helpers (bodies TODO) ──────────────────────────────────

-- Token actions — opaque tags so a verify-email token can't be
-- replayed as a password-reset token (the action is in the signed
-- payload and re-checked at consume time).
local ACTIONS = {
    verify_email      = "verify",
    password_reset    = "reset",
    magic_link        = "magic",
    email_change      = "email_change",
    -- Sent to the OLD address on an email-change request so the
    -- old-address holder can cancel a hostile change within TTL.
    email_change_revoke = "email_change_revoke",
    -- Pending 2FA token, issued after a successful first-factor
    -- check (password or magic-link click). Single-use ON SUCCESS,
    -- multi-use within TTL until then (lets users retry typos).
    totp_pending      = "totp_pending",
}

-- Tolerate either `user.id` (canonical) or `user.user_id` (legacy)
-- on app-supplied user objects. Centralized so the contract is one
-- line to revisit if we ever want to harden it. JS mirrors this
-- with `userId(user)` in stdlib/js/hull/web/auth-flows.js.
local function user_uid(user)
    if type(user) ~= "table" then return nil end
    return user.id or user.user_id
end

-- Signature framing (base64url(JSON) || "." || hex(HMAC)) lives
-- in hull.crypto.envelope so the malformed-hex pcall, the body/
-- tag split, and the vague-reason mapping aren't redone here.
-- The action-tag, expiry, and single-use bookkeeping are
-- auth-flows concerns and stay in this file.
local function issue_token(user_id, action, ttl, extra)
    local payload = {
        sub    = user_id,
        action = action,
        exp    = time.now() + ttl,
        -- 16 random bytes (base64url-encoded). Defends against
        -- guessable collisions and lets two tokens issued in the
        -- same second still be distinct.
        nonce  = crypto.base64url_encode(crypto.random(16)),
    }
    if extra then
        for k, v in pairs(extra) do payload[k] = v end
    end
    return envelope.sign(payload, _state.state_secret_hex)
end

-- Verify a token's signature + action + expiry WITHOUT marking
-- it used. Returns (envelope, nil) or (nil, reason). Reason
-- strings are intentionally vague at the response layer so an
-- attacker can't tell "tampered" from "expired".
--
-- Most flows wrap this in consume_token below (atomic verify +
-- mark-used). The TOTP-pending flow uses parse_token directly so
-- the token stays usable across retry-on-typo attempts and is
-- only burned on a successful code verify.
local function parse_token(token, expected_action)
    local env, err = envelope.verify(token, _state.state_secret_hex)
    if not env then return nil, err end
    if env.action ~= expected_action then return nil, "wrong action" end
    if type(env.exp) ~= "number" or time.now() >= env.exp then
        return nil, "expired"
    end
    return env, nil
end

-- Insert this token into the used set. Returns true if the
-- insert won (first use) and false if a row already existed
-- (replay / second consumer in a race).
local function mark_token_used(token, exp)
    local token_hash = crypto.sha256(token)
    local rc = db.exec(
        "INSERT OR IGNORE INTO _hull_auth_used_tokens "
        .. "(token_hash, used_at, expires_at) VALUES (?, ?, ?)",
        { token_hash, time.now(), exp })
    return rc > 0
end

local function token_already_used(token)
    local token_hash = crypto.sha256(token)
    local rows = db.query(
        "SELECT 1 FROM _hull_auth_used_tokens WHERE token_hash = ? LIMIT 1",
        { token_hash })
    return rows ~= nil and #rows > 0
end

-- Atomic verify + mark-used. The atomicity matters for the
-- click-through flows (verify, magic-link, reset, email-change)
-- because two concurrent clicks of the same link must not both
-- succeed.
local function consume_token(token, expected_action)
    local env, err = parse_token(token, expected_action)
    if err then return nil, err end
    if not mark_token_used(token, env.exp) then
        return nil, "replayed"
    end
    return env, nil
end

-- Templates: app provides functions returning { subject, html, text }.
-- We require subject + at least one of html/text; the email_send
-- callback decides which content type(s) to actually use.
local function render_template(name, ctx)
    local tpl = _state.templates[name]
    if type(tpl) ~= "function" then
        error("auth-flows: template '" .. tostring(name) .. "' not provided in init.templates")
    end
    local r = tpl(ctx)
    if type(r) ~= "table" or type(r.subject) ~= "string"
       or (type(r.html) ~= "string" and type(r.text) ~= "string") then
        error("auth-flows: template '" .. name
              .. "' must return { subject, html?, text? }")
    end
    return r
end

local function send_email(to, template_name, ctx)
    local r = render_template(template_name, ctx)
    _state.email_send(to, r.subject, r.html, r.text)
end

-- Cheap GC. Called opportunistically from the request path after
-- a successful confirm so the consumed-token table doesn't grow
-- unboundedly. Apps that want determinism can also schedule
-- gc_expired() via app.daily().
local function gc_expired()
    local now = time.now()
    db.exec("DELETE FROM _hull_auth_used_tokens WHERE expires_at < ?", { now })
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE expires_at < ?",
            { now })
    -- Clear lockout rows whose window has fully elapsed AND no
    -- recent failures (failed_count == 0 OR last_failed_at older
    -- than 1 day) so the table doesn't bloat over time.
    db.exec(
        "DELETE FROM _hull_auth_login_attempts "
        .. "WHERE (locked_until IS NULL OR locked_until < ?) "
        .. "  AND (last_failed_at IS NULL OR last_failed_at < ?)",
        { now, now - 86400 })
end

-- ── Lockout helpers ────────────────────────────────────────────────
--
-- Returns seconds-remaining on the lockout window (> 0) if user is
-- currently locked, or 0 / nil otherwise. Returning the integer lets
-- handle_login emit a Retry-After header without an extra query.
local function lockout_remaining(user_id)
    local rows = db.query(
        "SELECT locked_until FROM _hull_auth_login_attempts WHERE user_id = ?",
        { user_id })
    if not rows or #rows == 0 then return 0 end
    local lu = rows[1].locked_until
    if not lu or lu == 0 then return 0 end
    local now = time.now()
    if lu > now then return lu - now end
    return 0
end

local function bump_failed_login(user_id)
    local now = time.now()
    -- UPSERT: increment counter, set last_failed_at, set
    -- locked_until if threshold tripped. SQLite's
    -- INSERT ... ON CONFLICT supports this cleanly.
    db.exec(
        "INSERT INTO _hull_auth_login_attempts "
        .. "(user_id, failed_count, last_failed_at, locked_until) "
        .. "VALUES (?, 1, ?, NULL) "
        .. "ON CONFLICT(user_id) DO UPDATE SET "
        .. "  failed_count = failed_count + 1, "
        .. "  last_failed_at = ?, "
        .. "  locked_until = CASE WHEN failed_count + 1 >= ? "
        .. "                      THEN ? + ? ELSE NULL END",
        { user_id, now, now,
          _state.max_failed_logins,
          now, _state.lockout_duration })
end

local function clear_failed_logins(user_id)
    db.exec("DELETE FROM _hull_auth_login_attempts WHERE user_id = ?",
            { user_id })
end

-- ── Pwned-password check (opt-in) ──────────────────────────────────
--
-- Wraps hull.web.pwned with the init-time options. Returns true if
-- the password is in the breach corpus (caller should reject);
-- false otherwise (including HIBP-outage fail-open).
local function check_pwned(password)
    if not _state.check_pwned_passwords then return false end
    local pwned = require("hull.web.pwned")
    return pwned.check(password,
        _state.pwned_endpoint and { endpoint = _state.pwned_endpoint } or nil)
end

-- ── Body parsing ───────────────────────────────────────────────────
-- Accept either JSON or url-encoded form. Returns the parsed table
-- or {} on parse failure (the route handler does field-presence
-- validation separately).
local function parse_body(req)
    local body = req.body or ""
    if #body == 0 then return {} end
    local ct = (req.headers and req.headers["content-type"]) or ""
    if ct:find("application/json", 1, true) then
        local ok, t = pcall(json.decode, body)
        if ok and type(t) == "table" then return t end
        return {}
    end
    -- url-encoded fallback.
    local out = {}
    for pair in body:gmatch("[^&]+") do
        local eq = pair:find("=", 1, true)
        if eq then
            local k = pair:sub(1, eq - 1)
            local v = pair:sub(eq + 1):gsub("+", " "):gsub("%%(%x%x)",
                function(h) return string.char(tonumber(h, 16)) end)
            out[k] = v
        end
    end
    return out
end

-- Trivial email shape check. The actual deliverability is
-- determined by the email provider — we just guard against
-- obviously-garbage input.
local function is_email_ish(s)
    if type(s) ~= "string" then return false end
    if #s < 3 or #s > 254 then return false end
    local at = s:find("@", 1, true)
    if not at or at == 1 or at == #s then return false end
    local dot = s:find(".", at, true)
    if not dot or dot == at + 1 or dot == #s then return false end
    return true
end

-- Generic response shape for enumeration-safe endpoints. Same on
-- success and on "user doesn't exist" so an attacker can't
-- distinguish.
local function generic_ok(res)
    res:json({ ok = true })
end

-- ── Route handlers ─────────────────────────────────────────────────

local function handle_register(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) then
        return res:status(400):json({ error = "invalid email" })
    end
    if type(body.password) ~= "string" or #body.password < 8 then
        return res:status(400):json({ error = "password too short" })
    end
    -- Pwned-password check runs BEFORE user_find_by_email so a
    -- breached password is rejected with the same error regardless
    -- of whether the email already exists — enumeration-safe.
    if check_pwned(body.password) then
        return res:status(400):json({
            error = "password appears in known data breaches; choose another",
        })
    end
    -- Enumeration-safe: returns ok whether the email exists or not.
    -- If it does exist, no email goes out (we don't want to spam
    -- existing users, and we don't want to leak existence).
    local existing = _state.user_find_by_email(body.email)
    if existing then return generic_ok(res) end

    local pw_hash = crypto.hash_password(body.password)
    local user_id = _state.user_create(body.email, pw_hash)
    local user = _state.user_get(user_id)
    if not user then
        return res:status(500):json({ error = "user_create returned an id that user_get cannot resolve" })
    end

    local token = issue_token(user_id,
        ACTIONS.verify_email, _state.verify_ttl)
    local verify_url = (req.headers["x-forwarded-proto"] or "http")
        .. "://" .. (req.headers["x-forwarded-host"] or req.headers.host or "localhost")
        .. _state.prefix .. "/verify?token=" .. token

    send_email(body.email, "welcome", {
        user = user, verify_url = verify_url, token = token,
    })
    res:json({ ok = true })
end

-- POST /auth/verify/resend { email } — re-issue the welcome /
-- verify email if the address belongs to an UNVERIFIED user.
-- Enumeration-safe: always returns {ok:true} regardless of whether
-- the user exists or is already verified, so an attacker can't
-- learn which addresses are in the system or which still need
-- verification. Apps SHOULD rate-limit this route (the standard
-- ratelimit.middleware keyed by email body field works well).
local function handle_verify_resend(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) then
        return res:status(400):json({ error = "invalid email" })
    end
    local user = _state.user_find_by_email(body.email)
    if not user or user.email_verified then return generic_ok(res) end
    local user_id = user_uid(user)
    local token = issue_token(user_id, ACTIONS.verify_email,
                               _state.verify_ttl)
    local verify_url = (req.headers["x-forwarded-proto"] or "http")
        .. "://" .. (req.headers["x-forwarded-host"] or req.headers.host or "localhost")
        .. _state.prefix .. "/verify?token=" .. token
    send_email(body.email, "welcome", {
        user = user, verify_url = verify_url, token = token,
    })
    res:json({ ok = true })
end

local function handle_verify(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.verify_email)
    if not env then
        return res:status(400):html("verification failed: " .. (err or "?"))
    end
    _state.user_set_email_verified(env.sub, true)
    gc_expired()
    res:redirect(_state.verify_redirect)
end

-- Build the minimal default HTML form rendered when a magic-link
-- click lands and 2FA is required but the app hasn't configured a
-- custom `totp_pending_redirect`. The only interpolated value is
-- the pending token (HMAC base64url + hex tag — fixed alphabet),
-- so no escaping concerns; we keep it ugly-but-functional so apps
-- that care about UX point totp_pending_redirect at their own
-- page.
local function default_totp_form_html(token)
    return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        .. '<title>Two-factor verification</title></head>'
        .. '<body style="font-family:sans-serif;max-width:360px;'
        .. 'margin:4em auto;"><h1>Two-factor verification</h1>'
        .. '<form method="POST" action="' .. _state.prefix .. '/totp-verify">'
        .. '<input type="hidden" name="token" value="' .. token .. '">'
        .. '<p><label>Code: <input name="code" autofocus '
        .. 'autocomplete="one-time-code" inputmode="numeric" '
        .. 'pattern="[0-9A-Za-z-]+"></label></p>'
        .. '<button type="submit">Verify</button></form>'
        .. '<p style="color:#666;font-size:smaller">Lost your device? '
        .. 'Enter a recovery code instead.</p></body></html>'
end

-- Issue a pending-2FA token and respond appropriately for the
-- channel. POST (JSON login) → JSON; GET (magic-link click) →
-- HTML form or redirect.
local function start_totp_pending(req, res, user)
    local uid = user_uid(user)
    local token = issue_token(uid, ACTIONS.totp_pending,
                               _state.totp_pending_ttl)
    if req.method == "POST" then
        return res:json({
            ok = true, pending_2fa = true, totp_token = token,
        })
    end
    -- Browser GET (magic-link consume).
    if _state.totp_pending_redirect then
        local sep = _state.totp_pending_redirect:find("?", 1, true)
                    and "&" or "?"
        return res:redirect(_state.totp_pending_redirect
                             .. sep .. "token=" .. token)
    end
    res:html(default_totp_form_html(token))
end

local function handle_login(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) or type(body.password) ~= "string" then
        return res:status(400):json({ error = "invalid credentials" })
    end
    local user = _state.user_find_by_email(body.email)
    -- Lockout check runs BEFORE the password check so a known-locked
    -- account doesn't leak whether the password attempt was right or
    -- wrong via response timing. We compute it only when the user
    -- exists; for unknown emails the 401 below stays enumeration-safe.
    if user then
        local remain = lockout_remaining(user_uid(user))
        if remain > 0 then
            res:header("Retry-After", tostring(remain))
            return res:status(429):json({
                error = "too many failed attempts",
                retry_after = remain,
            })
        end
    end
    if not user or not user.password_hash
       or not crypto.verify_password(body.password, user.password_hash) then
        if user then bump_failed_login(user_uid(user)) end
        return res:status(401):json({ error = "invalid credentials" })
    end
    if _state.require_verified_email and not user.email_verified then
        return res:status(403):json({ error = "email not verified" })
    end
    -- Successful auth — clear the failed-attempts row so subsequent
    -- typos don't accumulate against a long-standing baseline.
    clear_failed_logins(user_uid(user))
    if _state.enable_totp
       and _state.user_totp_enrolled(user_uid(user)) then
        return start_totp_pending(req, res, user)
    end
    _state.on_login(req, res, user)
end

local function handle_logout(req, res)
    if _state.on_logout then
        _state.on_logout(req, res)
    else
        res:redirect("/")
    end
end

local function handle_magic_link(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) then
        return res:status(400):json({ error = "invalid email" })
    end
    local user = _state.user_find_by_email(body.email)
    if not user then
        if not _state.magic_link_auto_signup then
            -- Enumeration-safe: silently succeed without sending.
            return generic_ok(res)
        end
        -- Opt-in passwordless signup. Create the user with a NULL
        -- password_hash; the app's user_create must accept that.
        local user_id = _state.user_create(body.email, nil)
        user = _state.user_get(user_id)
    end
    local token = issue_token(user_uid(user),
        ACTIONS.magic_link, _state.magic_link_ttl)
    local link = (req.headers["x-forwarded-proto"] or "http")
        .. "://" .. (req.headers["x-forwarded-host"] or req.headers.host or "localhost")
        .. _state.prefix .. "/magic-link/consume?token=" .. token
    send_email(body.email, "magic_link", {
        user = user, link = link, token = token,
    })
    res:json({ ok = true })
end

local function handle_magic_link_consume(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.magic_link)
    if not env then
        return res:status(400):html("magic link failed: " .. (err or "?"))
    end
    local user = _state.user_get(env.sub)
    if not user then
        return res:status(400):html("magic link failed")
    end
    -- Magic-link clicks count as proof of email ownership.
    if not user.email_verified then
        _state.user_set_email_verified(user_uid(user), true)
        user.email_verified = true
    end
    gc_expired()
    if _state.enable_totp
       and _state.user_totp_enrolled(user_uid(user)) then
        return start_totp_pending(req, res, user)
    end
    _state.on_login(req, res, user)
end

-- POST /auth/totp-verify { token, code } — second factor.
-- The pending token is NOT consumed on a failed code attempt
-- (apps must rate-limit this route to bound retry; see the
-- module header for the recommended ratelimit.middleware
-- snippet). On success it's burned exactly like a single-use
-- token, then on_login runs.
local function handle_totp_verify(req, res)
    if not _state.enable_totp then
        return res:status(404):json({ error = "totp not enabled" })
    end
    local body = parse_body(req)
    if type(body.token) ~= "string" or type(body.code) ~= "string" then
        return res:status(400):json({ error = "missing token or code" })
    end
    local env, err = parse_token(body.token, ACTIONS.totp_pending)
    if not env then
        return res:status(400):json({ error = "totp failed: " .. (err or "?") })
    end
    if token_already_used(body.token) then
        return res:status(400):json({ error = "totp token already used" })
    end
    local user = _state.user_get(env.sub)
    if not user then
        return res:status(400):json({ error = "totp failed" })
    end
    local ok = _state.totp_verify(user, body.code)
    if not ok then
        return res:status(401):json({ error = "invalid code" })
    end
    mark_token_used(body.token, env.exp)
    gc_expired()
    _state.on_login(req, res, user)
end

local function handle_password_reset_request(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) then
        return res:status(400):json({ error = "invalid email" })
    end
    local user = _state.user_find_by_email(body.email)
    if not user then return generic_ok(res) end
    local token = issue_token(user_uid(user),
        ACTIONS.password_reset, _state.reset_ttl)
    local link = (req.headers["x-forwarded-proto"] or "http")
        .. "://" .. (req.headers["x-forwarded-host"] or req.headers.host or "localhost")
        .. _state.prefix .. "/password-reset/confirm?token=" .. token
    send_email(body.email, "password_reset", {
        user = user, link = link, token = token,
    })
    res:json({ ok = true })
end

local function handle_password_reset_confirm(req, res)
    local body = parse_body(req)
    if type(body.password) ~= "string" or #body.password < 8 then
        return res:status(400):json({ error = "password too short" })
    end
    -- Same pwned-password gate as register so a reset can't be used
    -- to land on a breached password.
    if check_pwned(body.password) then
        return res:status(400):json({
            error = "password appears in known data breaches; choose another",
        })
    end
    local env, err = consume_token(body.token, ACTIONS.password_reset)
    if not env then
        return res:status(400):json({ error = "reset failed: " .. (err or "?") })
    end
    local user = _state.user_get(env.sub)
    if not user then
        return res:status(400):json({ error = "reset failed" })
    end
    _state.user_set_password(env.sub, crypto.hash_password(body.password))
    -- A successful reset also unlocks the account: the user
    -- demonstrably controls the email, so any prior lockout is
    -- moot. (If they don't reset, the lockout window expires
    -- naturally per lockout_duration.)
    clear_failed_logins(env.sub)
    gc_expired()
    res:json({ ok = true })
end

local function handle_email_change(req, res)
    -- This route assumes the app has authenticated the request
    -- (e.g. via auth.session_middleware) and stashed the user id
    -- on req.ctx.user_id. The module doesn't depend on a specific
    -- session shape — apps wire this in.
    local user_id = req.ctx and req.ctx.user_id
    if not user_id then
        return res:status(401):json({ error = "not authenticated" })
    end
    local body = parse_body(req)
    if not is_email_ish(body.new_email) then
        return res:status(400):json({ error = "invalid email" })
    end
    -- Reject if the target email is already taken — reveals
    -- existence, but that's a UX call (the alternative is a silent
    -- accept that confuses the user).
    if _state.user_find_by_email(body.new_email) then
        return res:status(409):json({ error = "email already in use" })
    end

    local now = time.now()
    local token = issue_token(user_id, ACTIONS.email_change,
        _state.email_change_ttl, { new_email = body.new_email })
    local token_hash = crypto.sha256(token)
    db.exec(
        "INSERT OR REPLACE INTO _hull_auth_pending_email_changes "
        .. "(user_id, new_email, token_hash, created_at, expires_at) "
        .. "VALUES (?, ?, ?, ?, ?)",
        { user_id, body.new_email, token_hash, now,
          now + _state.email_change_ttl })

    local user = _state.user_get(user_id)
    local origin = (req.headers["x-forwarded-proto"] or "http")
        .. "://" .. (req.headers["x-forwarded-host"] or req.headers.host or "localhost")
    local link = origin .. _state.prefix .. "/email-change/confirm?token=" .. token
    -- Send to the NEW address — proves the user controls it.
    send_email(body.new_email, "email_change", {
        user = user, link = link, token = token,
        new_email = body.new_email,
    })
    -- Defense in depth: notify the OLD address with a revoke link
    -- so a stolen session cookie can't quietly move the account.
    -- Opt-in by providing templates.email_change_notify; apps that
    -- don't have the template keep the v1 behavior.
    if _state.templates.email_change_notify then
        local revoke_tok = issue_token(user_id, ACTIONS.email_change_revoke,
                                        _state.email_change_ttl)
        local revoke_url = origin .. _state.prefix
            .. "/email-change/revoke?token=" .. revoke_tok
        send_email(user.email, "email_change_notify", {
            user = user, revoke_url = revoke_url, revoke_token = revoke_tok,
            new_email = body.new_email,
        })
    end
    res:json({ ok = true })
end

-- GET /auth/email-change/revoke?token=... — consumed by the
-- OLD-address holder to cancel a pending email change. Single-use
-- via the same _hull_auth_used_tokens table; deletes the pending
-- row and (for paranoia) burns any matching email_change token
-- whose envelope is still in flight by deleting BOTH rows.
local function handle_email_change_revoke(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.email_change_revoke)
    if not env then
        return res:status(400):html("revoke failed: " .. (err or "?"))
    end
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            { env.sub })
    gc_expired()
    res:html("Email change canceled.")
end

local function handle_email_change_confirm(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.email_change)
    if not env then
        return res:status(400):html("email change failed: " .. (err or "?"))
    end
    local user = _state.user_get(env.sub)
    if not user then
        return res:status(400):html("email change failed")
    end
    -- Double-check there's a pending row and that the new_email
    -- in the envelope matches it (defense in depth — the token
    -- envelope IS the authority but a stale pending row should
    -- still get cleaned up).
    local rows = db.query(
        "SELECT new_email FROM _hull_auth_pending_email_changes "
        .. "WHERE user_id = ?", { env.sub })
    if not rows or #rows == 0
       or rows[1].new_email ~= env.new_email then
        return res:status(400):html("email change failed")
    end
    _state.user_set_email(env.sub, env.new_email)
    _state.user_set_email_verified(env.sub, true)
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            { env.sub })
    gc_expired()
    res:redirect(_state.verify_redirect)
end

-- Route registration helper.
local function register_routes(app)
    local p = _state.prefix
    app.post(p .. "/register",                 handle_register)
    app.get (p .. "/verify",                   handle_verify)
    app.post(p .. "/verify/resend",            handle_verify_resend)
    app.post(p .. "/login",                    handle_login)
    app.post(p .. "/logout",                   handle_logout)
    app.post(p .. "/magic-link",               handle_magic_link)
    app.get (p .. "/magic-link/consume",       handle_magic_link_consume)
    app.post(p .. "/password-reset/request",   handle_password_reset_request)
    app.post(p .. "/password-reset/confirm",   handle_password_reset_confirm)
    app.post(p .. "/email-change",             handle_email_change)
    app.get (p .. "/email-change/confirm",     handle_email_change_confirm)
    app.get (p .. "/email-change/revoke",      handle_email_change_revoke)
    -- Always registered so the route doesn't 404 with a confusing
    -- "no such route" when an app forgets enable_totp; the handler
    -- itself returns 404 with a clear error in that case.
    app.post(p .. "/totp-verify",              handle_totp_verify)
end

-- ── Public API ─────────────────────────────────────────────────────

--- Initialize the module. Must be called once at app startup.
-- @tparam table opts See module header for the full option list.
function M.init(opts)
    opts = opts or {}
    if type(opts.state_secret) ~= "string" or #opts.state_secret < 32 then
        error("auth-flows.init: state_secret must be a string >= 32 bytes")
    end
    if type(opts.email_send) ~= "function" then
        error("auth-flows.init: email_send(to, subject, html, text) required")
    end
    if type(opts.templates) ~= "table" then
        error("auth-flows.init: templates table required")
    end
    -- Required user-storage callbacks. Collected up front so the
    -- error message names them all rather than failing on the
    -- first missing one at request time.
    local required_user = {
        "user_find_by_email", "user_get", "user_create",
        "user_set_password", "user_set_email",
        "user_set_email_verified",
    }
    local missing = {}
    for _, k in ipairs(required_user) do
        if type(opts[k]) ~= "function" then
            missing[#missing + 1] = k
        end
    end
    if #missing > 0 then
        error("auth-flows.init: missing required callbacks: "
              .. table.concat(missing, ", "))
    end
    if type(opts.on_login) ~= "function" then
        error("auth-flows.init: on_login(req, res, user) required")
    end
    if opts.enable_totp then
        if type(opts.user_totp_enrolled) ~= "function" then
            error("auth-flows.init: user_totp_enrolled(user_id) -> "
                  .. "boolean required when enable_totp = true")
        end
        if type(opts.totp_verify) ~= "function" then
            error("auth-flows.init: totp_verify(user, code) -> boolean "
                  .. "required when enable_totp = true")
        end
    end

    -- crypto.hmac_sha256 takes the key as a hex string; we encode
    -- once at init and reuse the hex form per request.
    _state.state_secret_hex = crypto.hex_encode(opts.state_secret)
    _state.email_send       = opts.email_send
    _state.templates        = opts.templates
    _state.user_find_by_email      = opts.user_find_by_email
    _state.user_get                = opts.user_get
    _state.user_create             = opts.user_create
    _state.user_set_password       = opts.user_set_password
    _state.user_set_email          = opts.user_set_email
    _state.user_set_email_verified = opts.user_set_email_verified
    _state.on_login                = opts.on_login
    _state.on_logout               = opts.on_logout
    _state.enable_totp             = opts.enable_totp == true
    _state.user_totp_enrolled      = opts.user_totp_enrolled
    _state.totp_verify             = opts.totp_verify
    _state.totp_pending_ttl        = opts.totp_pending_ttl
                                     or _state.totp_pending_ttl
    _state.totp_pending_redirect   = opts.totp_pending_redirect
                                     or _state.totp_pending_redirect
    _state.max_failed_logins       = opts.max_failed_logins
                                     or _state.max_failed_logins
    _state.lockout_duration        = opts.lockout_duration
                                     or _state.lockout_duration
    _state.check_pwned_passwords   = opts.check_pwned_passwords == true
    _state.pwned_endpoint          = opts.pwned_endpoint
    _state.verify_ttl       = opts.verify_ttl       or _state.verify_ttl
    _state.reset_ttl        = opts.reset_ttl        or _state.reset_ttl
    _state.magic_link_ttl   = opts.magic_link_ttl   or _state.magic_link_ttl
    _state.email_change_ttl = opts.email_change_ttl or _state.email_change_ttl
    _state.prefix           = opts.prefix           or _state.prefix
    _state.verify_redirect  = opts.verify_redirect  or _state.verify_redirect
    _state.login_redirect   = opts.login_redirect   or _state.login_redirect
    if opts.enumeration_safe ~= nil then
        _state.enumeration_safe = opts.enumeration_safe
    end
    if opts.magic_link_auto_signup ~= nil then
        _state.magic_link_auto_signup = opts.magic_link_auto_signup
    end
    if opts.require_verified_email ~= nil then
        _state.require_verified_email = opts.require_verified_email
    end

    db.batch(function()
        for stmt in SCHEMA:gmatch("([^;]+);") do
            local s = stmt:gsub("^%s+", ""):gsub("%s+$", "")
            if #s > 0 then db.exec(s) end
        end
    end)

    _state._initialized = true
end

--- Mount all auth-flow routes under `opts.prefix` (default `/auth`).
-- @tparam table app
function M.routes(app)
    if not _state._initialized then
        error("auth-flows.routes: call auth-flows.init() first")
    end
    register_routes(app)
end

--- Standalone helpers — useful when an app needs to trigger one of
--- the flows from outside the standard routes (e.g. an admin
--- forcing a password reset for a user).

--- Programmatically trigger a verify-email send. Useful for admin
--- panels resending the link, or wiring this into a "re-send"
--- button on the app's login page. `verify_url_prefix` is the
--- full origin (`"https://app.example.com"`) — the module can't
--- know the public URL the user accesses without a request to
--- read its `Host`/`X-Forwarded-Host` from.
function M.send_verify_email(user, verify_url_prefix)
    if not _state._initialized then
        error("auth-flows: call init() before send_verify_email()")
    end
    if type(user) ~= "table" or not (user_uid(user)) then
        error("auth-flows.send_verify_email: user table with id required")
    end
    local user_id = user_uid(user)
    local token = issue_token(user_id, ACTIONS.verify_email,
                               _state.verify_ttl)
    local verify_url = (verify_url_prefix or "")
                       .. _state.prefix .. "/verify?token=" .. token
    send_email(user.email, "welcome", {
        user = user, verify_url = verify_url, token = token,
    })
end

function M.send_password_reset(email, reset_url_prefix)
    if not _state._initialized then
        error("auth-flows: call init() before send_password_reset()")
    end
    if not is_email_ish(email) then
        error("auth-flows.send_password_reset: invalid email")
    end
    local user = _state.user_find_by_email(email)
    if not user then return end  -- enumeration-safe; silently no-op
    local user_id = user_uid(user)
    local token = issue_token(user_id, ACTIONS.password_reset,
                               _state.reset_ttl)
    local link = (reset_url_prefix or "")
                 .. _state.prefix .. "/password-reset/confirm?token=" .. token
    send_email(email, "password_reset", {
        user = user, link = link, token = token,
    })
end

function M.send_magic_link(email, magic_url_prefix)
    if not _state._initialized then
        error("auth-flows: call init() before send_magic_link()")
    end
    if not is_email_ish(email) then
        error("auth-flows.send_magic_link: invalid email")
    end
    local user = _state.user_find_by_email(email)
    if not user then
        if not _state.magic_link_auto_signup then return end
        local user_id = _state.user_create(email, nil)
        user = _state.user_get(user_id)
    end
    local user_id = user_uid(user)
    local token = issue_token(user_id, ACTIONS.magic_link,
                               _state.magic_link_ttl)
    local link = (magic_url_prefix or "")
                 .. _state.prefix .. "/magic-link/consume?token=" .. token
    send_email(email, "magic_link", {
        user = user, link = link, token = token,
    })
end

-- ── Test helpers (not public; exposed for unit tests) ──────────────

M._test = {
    issue_token        = issue_token,
    consume_token      = consume_token,
    parse_token        = parse_token,
    mark_token_used    = mark_token_used,
    token_already_used = token_already_used,
    render_template    = render_template,
    gc_expired         = gc_expired,
    is_email_ish       = is_email_ish,
    parse_body         = parse_body,
    ACTIONS            = ACTIONS,
    reset = function()
        _state.state_secret_hex = nil
        _state.email_send       = nil
        _state.templates        = {}
        _state.user_find_by_email      = nil
        _state.user_get                = nil
        _state.user_create             = nil
        _state.user_set_password       = nil
        _state.user_set_email          = nil
        _state.user_set_email_verified = nil
        _state.on_login                = nil
        _state.on_logout               = nil
        _state.enable_totp             = false
        _state.user_totp_enrolled      = nil
        _state.totp_verify             = nil
        _state.totp_pending_redirect   = nil
        _state.check_pwned_passwords   = false
        _state.pwned_endpoint          = nil
        _state.max_failed_logins       = 5
        _state.lockout_duration        = 15 * 60
        _state._initialized            = false
    end,
}

return M
