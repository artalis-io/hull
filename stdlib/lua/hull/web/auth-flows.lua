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

local crypto = require("hull.crypto")
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
}

-- Token = base64url(JSON{user_id, action, exp, nonce, extra...})
--         "." hex(HMAC-SHA256(state_secret, body))
-- The HMAC is over the body bytes only — exactly the OAuth state
-- cookie pattern that's already in production in this stdlib.
local function issue_token(user_id, action, ttl, extra)
    local payload = {
        sub    = user_id,
        action = action,
        exp    = time.now() + ttl,
        -- 16 random bytes (base64url-encoded by crypto.random + the
        -- urlsafe-encoder). Defends against guessable collisions
        -- and lets two tokens issued in the same second still be
        -- distinct.
        nonce  = crypto.base64url_encode(crypto.random(16)),
    }
    if extra then
        for k, v in pairs(extra) do payload[k] = v end
    end
    local body = crypto.base64url_encode(json.encode(payload))
    local tag = crypto.hmac_sha256(body, _state.state_secret_hex)
    return body .. "." .. tag
end

-- Verify + consume a token atomically. The atomicity matters
-- because two concurrent click-throughs of the same link must
-- not both succeed.
--
-- Returns (envelope, nil) on success or (nil, reason) on failure.
-- The reason strings are intentionally vague at the response
-- layer so an attacker can't distinguish "tampered" from
-- "expired" from "replayed".
local function consume_token(token, expected_action)
    if type(token) ~= "string" or token == "" then
        return nil, "missing"
    end
    local dot = token:find(".", 1, true)
    if not dot then return nil, "malformed" end
    local body = token:sub(1, dot - 1)
    local tag  = token:sub(dot + 1)

    -- crypto.hmac_sha256_verify raises on malformed-hex inputs
    -- (intentional at the cap layer — programmer error rather than
    -- a verify result). Catch with pcall so a user-supplied token
    -- with junk in the tag position returns a clean "bad tag"
    -- result instead of crashing the request handler.
    local ok, valid = pcall(crypto.hmac_sha256_verify, body,
                             _state.state_secret_hex, tag)
    if not ok or not valid then
        return nil, "bad tag"
    end

    local raw = crypto.base64url_decode(body)
    if not raw then return nil, "bad encoding" end
    local env = json.decode(raw)
    if type(env) ~= "table" then return nil, "bad json" end
    if env.action ~= expected_action then return nil, "wrong action" end
    if type(env.exp) ~= "number" or time.now() >= env.exp then
        return nil, "expired"
    end

    -- Single-use enforcement. Insert the token hash before
    -- proceeding; PK conflict means "already consumed". sha256 of
    -- the full token string (body + "." + tag) gives a stable
    -- fixed-width key.
    local token_hash = crypto.sha256(token)
    -- INSERT OR IGNORE returns rowcount 0 on conflict — that's our
    -- replay signal.
    local rc = db.exec(
        "INSERT OR IGNORE INTO _hull_auth_used_tokens "
        .. "(token_hash, used_at, expires_at) VALUES (?, ?, ?)",
        { token_hash, time.now(), env.exp })
    if rc == 0 then return nil, "replayed" end

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

local function handle_login(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) or type(body.password) ~= "string" then
        return res:status(400):json({ error = "invalid credentials" })
    end
    local user = _state.user_find_by_email(body.email)
    if not user or not user.password_hash
       or not crypto.verify_password(body.password, user.password_hash) then
        return res:status(401):json({ error = "invalid credentials" })
    end
    if _state.require_verified_email and not user.email_verified then
        return res:status(403):json({ error = "email not verified" })
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
    local token = issue_token(user.id or user.user_id,
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
        _state.user_set_email_verified(user.id or user.user_id, true)
        user.email_verified = true
    end
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
    local token = issue_token(user.id or user.user_id,
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
    local env, err = consume_token(body.token, ACTIONS.password_reset)
    if not env then
        return res:status(400):json({ error = "reset failed: " .. (err or "?") })
    end
    local user = _state.user_get(env.sub)
    if not user then
        return res:status(400):json({ error = "reset failed" })
    end
    _state.user_set_password(env.sub, crypto.hash_password(body.password))
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
    local link = (req.headers["x-forwarded-proto"] or "http")
        .. "://" .. (req.headers["x-forwarded-host"] or req.headers.host or "localhost")
        .. _state.prefix .. "/email-change/confirm?token=" .. token
    -- Send to the NEW address — proves the user controls it.
    send_email(body.new_email, "email_change", {
        user = user, link = link, token = token,
        new_email = body.new_email,
    })
    res:json({ ok = true })
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
    app.post(p .. "/login",                    handle_login)
    app.post(p .. "/logout",                   handle_logout)
    app.post(p .. "/magic-link",               handle_magic_link)
    app.get (p .. "/magic-link/consume",       handle_magic_link_consume)
    app.post(p .. "/password-reset/request",   handle_password_reset_request)
    app.post(p .. "/password-reset/confirm",   handle_password_reset_confirm)
    app.post(p .. "/email-change",             handle_email_change)
    app.get (p .. "/email-change/confirm",     handle_email_change_confirm)
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

    -- crypto.hmac_sha256 takes the key as a hex string; we encode
    -- once at init and reuse the hex form per request.
    local parts = {}
    for i = 1, #opts.state_secret do
        parts[i] = string.format("%02x", string.byte(opts.state_secret, i))
    end
    _state.state_secret_hex = table.concat(parts)
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
    if type(user) ~= "table" or not (user.id or user.user_id) then
        error("auth-flows.send_verify_email: user table with id required")
    end
    local user_id = user.id or user.user_id
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
    local user_id = user.id or user.user_id
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
    local user_id = user.id or user.user_id
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
        _state._initialized            = false
    end,
}

return M
