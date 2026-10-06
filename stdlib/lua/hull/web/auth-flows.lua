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
--   GET   /auth/verify?token=...         -- verify page (never consumes)
--   POST  /auth/verify                   -- {token, password | new_password}
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
--   * 2FA. The app composes `hull/web/middleware/totp` separately
--     by setting `enable_totp = true` + supplying `user_totp_enrolled`
--     and `totp_verify` callbacks. auth-flows then short-circuits a
--     successful password verify into POST /auth/totp-verify with a
--     `totp_pending` envelope token; the session is only created
--     once the code verifies. See `examples/auth_flows/app.lua`.
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
--     email is only sent if the address is registered. The login
--     path runs a dummy PBKDF2 verify on unknown emails so response
--     timing is identical to a known-but-wrong attempt (the previous
--     ~50-200ms delta would otherwise leak account existence over
--     the network). Controlled by `enumeration_safe` (default true).
--     Set false ONLY in test fixtures that don't care about timing
--     observables - never in production.
--   * Re-verify-on-email-change: changing email puts the new
--     address in `_hull_auth_pending_email_changes`; the row only
--     swaps onto the user record after the user clicks the link
--     sent to the new address. Old email stays active until then.
--     Starting a change takes the current password (or
--     email_change_reauth), and with templates.email_change_notify the
--     old address gets a revoke link that cancels the change - or, once
--     confirmed, restores the old address - and revokes every session.
--   * Login CSRF: every POST that signs a browser in or changes the
--     account refuses a cross-site request (see "Cross-site guard").
--
-- ## Usage
--
--     local authflows = require("hull.web.auth-flows")
--     authflows.init({
--         secret        = env.get("AUTH_FLOWS_SECRET"),  -- alias: state_secret
--         email_send    = function(to, subject, html, text) ... end,
--         templates     = {
--             welcome        = function(ctx) ... end,
--             verify         = function(ctx) ... end,
--             magic_link     = function(ctx) ... end,
--             password_reset = function(ctx) ... end,
--             email_change   = function(ctx) ... end,
--         },
--         -- user_find_by_email must return password_hash (login and the
--         -- verify step read it there); user_get need not. email_verified
--         -- is a boolean (0 / 1 are accepted).
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

local _text = require("hull._text")   -- linear trims (see hull._text)
local crypto    = require("hull.crypto")
local envelope  = require("hull.crypto.envelope")
local encoding = require("hull.encoding")
local db        = require("hull.db._internal").connection()
local time      = require("hull.time")
local json      = require("hull.json")
local pwned     = require("hull.web.pwned")
local audit_log = require("hull.web.middleware.audit-log")
local ratelimit = require("hull.web.middleware.ratelimit")
local _request  = require("hull.web._request")

local M = {}

-- ── Module state ───────────────────────────────────────────────────

local _state = {
    state_secret          = nil,
    -- TTLs in seconds. Verify and reset can be long; magic-link short.
    verify_ttl            = 86400,   -- 24h
    reset_ttl             = 3600,    -- 1h
    magic_link_ttl        = 600,     -- 10min
    email_change_ttl      = 86400,   -- 24h
    -- Mount prefix. Defaults to "/auth"; apps in mixed-routing setups
    -- can change this.
    prefix                = "/auth",
    -- Round-9 HIGH-1: URL origin gate. EVERY click-through URL (verify,
    -- magic-link, password-reset, email-change) is interpolated into a
    -- link inside an outbound email. Pre-round-9, the origin came
    -- straight from req.headers["x-forwarded-host"] / req.headers.host
    -- with no validation - an attacker submitting password-reset/request
    -- for victim's email with `Host: attacker.com` got the victim a
    -- "reset your password" mail pointing at attacker.com/auth/...?token=
    -- whose click leaked a valid action-bound reset token. Trivial
    -- account takeover.
    --
    -- Init now REQUIRES one of:
    --   * `public_origin = "https://app.example.com"` - single canonical
    --     URL; used as the sole authority for built links. The most
    --     common shape; the right answer for ~every non-multi-tenant app.
    --   * `trusted_hosts = {"app.example.com", "alt.example.com"}` -
    --     allowlist. req.headers.host must match (exactly) or the URL
    --     build refuses. Multi-tenant deployments where each customer
    --     has a different domain. The link is built from the matched
    --     entry, never from the request: a bare host gives a link
    --     without a port, and an entry "host:port" (which matches a
    --     request on that port only) gives that port.
    --   * `trust_request_host = true` - DEV/TEST escape hatch. Falls
    --     back to the pre-round-9 behaviour: uses req.headers.host
    --     directly. Logs a one-shot warning at init. NEVER pass this
    --     in production - that's exactly the foot-gun this gate exists
    --     to close. Test fixtures use this because they bind a random
    --     ephemeral port at startup and can't know the host at init.
    -- public_origin (if set) wins; otherwise the host header must
    -- match trusted_hosts; otherwise trust_request_host falls back;
    -- otherwise no URL is built and the response stays generic.
    public_origin         = nil,
    trusted_hosts         = nil,
    trust_request_host    = false,
    -- Honor X-Forwarded-Proto only behind a trusted proxy. Off by default so a
    -- spoofed header can't downgrade an emailed https link to http on a
    -- directly-exposed app; the per-branch scheme default is used otherwise.
    trust_proxy           = false,
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
    -- routes can decide what's gated. That mode requires
    -- on_password_reset (init refuses without it): it revokes a
    -- pre-registrant's sessions when the address owner verifies.
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
    -- The count is kept per (account, client IP): keyed on the account
    -- alone, five wrong passwords from anywhere locked anyone out. A
    -- second, account-wide count with a much higher threshold still stops
    -- a brute force spread over many addresses.
    max_failed_logins     = 5,
    max_failed_logins_per_account = 50,
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

    -- Flow-completion audit events (opt-in). When sign_in_log = true,
    -- auth-flows records password_reset_completed / email_change_revoked
    -- / email_changed into _hull_audit_log via hull/web/middleware/
    -- audit-log. Pair with on_password_reset to revoke existing
    -- sessions on reset (apps typically wire it as
    -- `function(req,res,user) session.destroy_all(user.id) end`).
    --
    -- LOGIN events and new-device detection are NOT emitted here
    -- anymore - they move to hull/web/middleware/session's
    -- login_handler factory (audit_log + on_new_device opts), so a
    -- single seam covers both password and OAuth logins.
    sign_in_log         = false,
    audit_log           = nil,   -- module handle, lazy-required when
                                 -- sign_in_log is enabled
    on_password_reset   = nil,

    -- Login rate limit (opt-in). When login_ratelimit is truthy, an
    -- hull/web/middleware/ratelimit middleware is installed on
    -- POST /auth/login BEFORE the handler. Defaults to 20 requests
    -- per IP per 5 minutes - tuned to make brute-force impractical
    -- without blocking power-users with sticky typos. Pass a table
    -- to override: { limit = N, window = SECONDS, key = function|str }.
    -- Apps with their own upstream rate-limiter should leave this off.
    login_ratelimit     = false,

    -- Per-recipient email send rate limit (ON by default). Gate
    -- inside send_email; blocked sends are silently dropped so the
    -- response stays enumeration-safe (matches the existing silent
    -- path for unknown-email magic-link / reset). Defends against
    -- the attacker-chosen-recipient email-storm class on
    -- /auth/email-change, /auth/magic-link, /auth/password-reset/
    -- request, and /auth/verify/resend. login_ratelimit (per-IP) is
    -- orthogonal - a botnet defeats per-IP but not per-recipient.
    --
    -- Shape: { limit = N, window = SECONDS }. Pass `false` to
    -- disable. In-memory sliding window; resets on restart.
    -- Bounded to email_rate_limit_max_entries unique recipients.
    email_rate_limit    = { limit = 3, window = 900 },
    email_rate_limit_max_entries = 10000,

    -- Optional post-action redirects.
    verify_redirect       = "/",
    -- GET /verify renders a default form (confirm the password, or set a new
    -- one). Set this to an app page to render your own: it is redirected to
    -- with ?token=... appended, and POSTs {token, password | new_password} to
    -- <prefix>/verify. See handle_verify.
    verify_form_redirect  = nil,
    -- `function(user_id)` that removes a TOTP enrolment (typically
    -- totp.disable). Called when the mailbox holder sets the password of an
    -- account that was not verified yet: see drop_preverify_totp.
    totp_disable          = nil,
    -- `function(req, user) -> true` when the request proves a recent sign-in
    -- of its own (or for passwordless accounts): POST /email-change then
    -- needs no current password. See handle_email_change.
    email_change_reauth   = nil,
    login_redirect        = "/",
    _initialized          = false,
}

-- ── Schema ─────────────────────────────────────────────────────────

local SCHEMA = [[
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
]]

-- ── Private helpers (bodies TODO) ──────────────────────────────────

-- Token actions - opaque tags so a verify-email token can't be
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

-- Password bounds: 8..256 characters, counted by codepoint as the JS twin
-- counts them (a byte count refused a 100-character CJK passphrase JS took),
-- plus a byte cap that keeps the PBKDF2 input bounded. Invalid UTF-8 counts
-- bytes. `min` is false where only the upper bound applies (a login attempt).
local PW_MIN, PW_MAX, PW_MAX_BYTES = 8, 256, 1024
local function password_len_ok(pw, min)
    if type(pw) ~= "string" or #pw > PW_MAX_BYTES then return false end
    local n = utf8.len(pw) or #pw
    return n <= PW_MAX and (min == false or n >= PW_MIN)
end

-- Only `true` or a non-zero number (the same test as JS's isVerified): an
-- adapter that returns raw rows hands back 0 / 1, and Lua treats 0 as true -
-- login, the verify step and the magic-link gate would all take an unverified
-- account for a verified one. A string ("0", "false") fails closed.
local function is_verified(user)
    local v = user and user.email_verified
    return v == true or (type(v) == "number" and v ~= 0)
end

-- The account's stored password hash, read the way /login reads it: through
-- user_find_by_email, whose contract carries password_hash. user_get need not
-- return it (keeping the hash out of the model is a common habit), and reading
-- it there made a missing field look like "no password". Returns the hash, nil
-- when the account has none, or false when it cannot be determined - callers
-- then treat the account as having one (fail closed).
local function stored_password_hash(user)
    local h = user and user.password_hash
    if type(h) == "string" and h ~= "" then return h end
    if type(user) ~= "table" or type(user.email) ~= "string" then return false end
    local found = _state.user_find_by_email(user.email)
    if type(found) ~= "table"
       or tostring(user_uid(found)) ~= tostring(user_uid(user)) then
        return false
    end
    h = found.password_hash
    if type(h) == "string" and h ~= "" then return h end
    return nil
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
        nonce  = crypto.random_token(16),
    }
    if extra then
        for k, v in pairs(extra) do payload[k] = v end
    end
    return envelope.sign(payload, _state.state_secret)
end

-- A reset token names the password it replaces: a short hash of the
-- account's password_hash at issue time, checked again at confirm. Once the
-- password changes (any reset, or a change), every other outstanding reset
-- link - a leaked one included - stops working, instead of living out its
-- reset_ttl. Read through user_find_by_email, the lookup login itself relies
-- on for password_hash, so issue and confirm see the same field.
-- The account's email is part of it too: a link sent to the old mailbox
-- stayed good after an email change (the password hash was unchanged), so
-- whoever still reads that mailbox could reset the password.
local function email_binding(user)
    local e = user and user.email
    return encoding.hex.encode(crypto.sha256(
        type(e) == "string" and e:lower() or "")):sub(1, 16)
end

local function password_binding(user)
    local h = user and user.password_hash
    return encoding.hex.encode(crypto.sha256(
        (type(h) == "string" and h or "") .. "\0" .. email_binding(user))):sub(1, 16)
end

local function reset_token_extra(user)
    return { pwb = password_binding(user) }
end

local function reset_binding_holds(env, user)
    local current = user and type(user.email) == "string"
                    and _state.user_find_by_email(user.email)
    return current ~= nil and current ~= false
           and env.pwb == password_binding(current)
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
    local env, err = envelope.verify(token, _state.state_secret)
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
    local token_hash = encoding.hex.encode(crypto.sha256(token))
    local rc = db.insert_if_absent(
        "_hull_auth_used_tokens",
        { "token_hash" },
        { "token_hash", "used_at", "expires_at" },
        { token_hash, time.now(), exp })
    return rc > 0
end

local function token_already_used(token)
    local token_hash = encoding.hex.encode(crypto.sha256(token))
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

-- Per-recipient email send rate limit. Sliding window keyed by
-- lower-cased recipient. Blocked sends are dropped silently so
-- the response shape stays enumeration-safe.
local _email_rl = {}
local _email_rl_count = 0
-- The size at which the next sweep runs. Raised past the table's size after
-- each sweep: saturated buckets survive a sweep, so with the threshold at
-- the cap every new recipient re-ran a full O(n) sweep (and sort).
local _email_rl_sweep_at = 0

local function email_rate_allow(to)
    local cfg = _state.email_rate_limit
    if not cfg or type(cfg) ~= "table" then return true end
    if type(to) ~= "string" or to == "" then return true end
    local key = to:lower()
    local now = time.now()
    local cutoff = now - (cfg.window or 900)
    local bucket = _email_rl[key]
    if not bucket then
        bucket = { ts = {} }
        _email_rl[key] = bucket
        _email_rl_count = _email_rl_count + 1
        -- Soft cap on table size: when over the max, drop the
        -- oldest buckets in one sweep. Anti-abuse memory bound;
        -- the legitimate working-set is small.
        if _email_rl_count > math.max(_email_rl_sweep_at,
                                      _state.email_rate_limit_max_entries or 10000) then
            local kept = {}
            local kept_n = 0
            for k, b in pairs(_email_rl) do
                local fresh = false
                for _, t in ipairs(b.ts) do
                    if t > cutoff then fresh = true; break end
                end
                if fresh then
                    kept[k] = b
                    kept_n = kept_n + 1
                end
            end
            _email_rl = kept
            -- Round-9 LOW-11: the just-created `bucket` was already
            -- registered in _email_rl_count (line 398), got evicted by
            -- the sweep (empty ts), and is re-added on the next line.
            -- Without the +1 the counter under-counts by 1 per sweep
            -- and the effective ceiling drifts above the configured
            -- max over many sweeps. JS path uses Map.size and is
            -- inherently correct.
            _email_rl_count = kept_n + 1
            _email_rl[key] = bucket
            -- Still over: a flood of distinct recipients, all inside the
            -- window, so the sweep kept them all - the table outgrew the cap
            -- and every new key paid an O(n) sweep. Drop the least recently
            -- used down to 90% of the cap, so the next sweep is a tenth of
            -- the cap away. The key just added is kept, and so is every
            -- bucket AT its limit: dropping one reset it, so flooding other
            -- addresses bought an attacker a fresh allowance against the
            -- address they were blocked on. A saturated bucket costs `limit`
            -- sends to create, so keeping them all stays bounded.
            local cap = _state.email_rate_limit_max_entries or 10000
            local limit = cfg.limit or 3
            if _email_rl_count > cap then
                local order = {}
                for k, b in pairs(_email_rl) do
                    local live = 0
                    for _, t in ipairs(b.ts) do
                        if t > cutoff then live = live + 1 end
                    end
                    if k ~= key and live < limit then
                        order[#order + 1] = { k = k, t = b.ts[#b.ts] or 0 }
                    end
                end
                table.sort(order, function(x, y) return x.t < y.t end)
                local target = math.floor(cap * 0.9)
                local i = 1
                while _email_rl_count > target and i <= #order do
                    _email_rl[order[i].k] = nil
                    _email_rl_count = _email_rl_count - 1
                    i = i + 1
                end
            end
            -- A hard ceiling: past twice the cap, saturated buckets go too,
            -- oldest first, down to the cap - memory stays bounded however
            -- many addresses an attacker saturates.
            if _email_rl_count > 2 * cap then
                local sat = {}
                for k, b in pairs(_email_rl) do
                    if k ~= key then sat[#sat + 1] = { k = k, t = b.ts[#b.ts] or 0 } end
                end
                table.sort(sat, function(x, y) return x.t < y.t end)
                local i = 1
                while _email_rl_count > cap and i <= #sat do
                    _email_rl[sat[i].k] = nil
                    _email_rl_count = _email_rl_count - 1
                    i = i + 1
                end
            end
            _email_rl_sweep_at = _email_rl_count + math.max(1, math.floor(cap / 10))
        end
    end
    local fresh = {}
    for _, t in ipairs(bucket.ts) do
        if t > cutoff then fresh[#fresh + 1] = t end
    end
    bucket.ts = fresh
    if #fresh >= (cfg.limit or 3) then return false end
    bucket.ts[#bucket.ts + 1] = now
    return true
end

-- Round-9 MEDIUM-6: strict allowlist (round-8's 1-field denylist
-- was the wrong shape - apps with `totp_secret`, `recovery_codes`,
-- `api_key`, `oauth_refresh_token` etc. on their user records
-- leaked them into on_login + email template ctx.user). Default
-- allowlist is the canonical auth surface: id, email,
-- email_verified. Apps that need to expose more (display_name,
-- avatar_url, roles, etc.) pass `user_sanitize = function(user)
-- return {...} end` which gets the raw user and returns what's
-- safe to surface. Breaking change for apps reading custom user
-- fields in templates / on_login - they must wire user_sanitize.
local SAFE_USER_FIELDS = {
    id             = true,
    user_id        = true,  -- legacy callers may use either
    email          = true,
    email_verified = true,
}

local function strip_user_secrets(user)
    if type(user) ~= "table" then return user end
    if _state.user_sanitize then
        local ok, sanitized = pcall(_state.user_sanitize, user)
        if ok and type(sanitized) == "table" then return sanitized end
        -- user_sanitize threw or returned non-table: fall back to the
        -- strict allowlist so the leak never reaches the boundary.
        local log = require("hull.log")
        log.warn("auth-flows: user_sanitize callback failed; falling "
              .. "back to strict allowlist")
    end
    local out = {}
    for k, v in pairs(user) do
        if SAFE_USER_FIELDS[k] then out[k] = v end
    end
    return out
end

-- The request's host, strictly: X-Forwarded-Host only behind a trusted
-- proxy (a direct client sets any header it likes), its first entry, and
-- nothing but a hostname or bracketed IPv6 literal plus an optional numeric
-- port. Returns host, port|nil - or nil. Anything else (userinfo, a path,
-- percent-encoding) is refused, never trimmed into shape.
local function request_host(h)
    local raw = (_state.trust_proxy and h["x-forwarded-host"]) or h.host
    if type(raw) ~= "string" then return nil end
    local comma = raw:find(",", 1, true)
    if comma then raw = raw:sub(1, comma - 1) end
    raw = _text.trim(raw)
    local host, rest = raw:match("^(%[[%x:%.]+%])(.*)$")
    if not host then host, rest = raw:match("^([%w%.%-]+)(.*)$") end
    if not host then return nil end
    local port
    if rest ~= "" then
        port = rest:match("^:(%d%d?%d?%d?%d?)$")
        if not port or tonumber(port) > 65535 then return nil end
    end
    return host, port
end

-- X-Forwarded-Proto behind a trusted proxy, and only "http" / "https".
local function request_proto(h, default)
    if _state.trust_proxy then
        local p = h["x-forwarded-proto"]
        if p == "http" or p == "https" then return p end
    end
    return default
end

-- Round-9 HIGH-1: build a click-through URL origin from validated
-- sources only. public_origin (when set) wins unconditionally;
-- otherwise the request's host must match a trusted_hosts entry
-- exactly, and the URL is built from THAT ENTRY alone - never from the
-- header. The request's port is not copied either (audit 7): a client
-- choosing it sent reset and magic-link tokens to whatever listened on
-- that port of the trusted host; an entry "host:port" pins one. A header
-- like "app.example.com:@evil.com" used to pass the allowlist (its host
-- part, cut at ':', matched) and then became the link itself, sending
-- reset and magic-link tokens to evil.com.
-- If neither check admits a value, return nil - the handler then answers
-- with the same enumeration-safe response without sending the link.
local function origin_for(req)
    if _state.public_origin then
        return _state.public_origin
    end
    local h = (req and req.headers) or {}
    local host, port = request_host(h)
    if host and _state.trusted_hosts then
        for _, allowed in ipairs(_state.trusted_hosts) do
            if host == allowed or (port and allowed == host .. ":" .. port) then
                return request_proto(h, "https") .. "://" .. allowed
            end
        end
    end
    -- trust_request_host opt-out (dev/test). Last resort; the init
    -- warning fires once so operators can spot it in startup logs.
    if host and _state.trust_request_host then
        return request_proto(h, "http") .. "://" .. host .. (port and (":" .. port) or "")
    end
    -- Round-11 LOW-10: one-shot warn the first time this fires, so a
    -- misconfigured trusted_hosts is not silent no-mail. The
    -- enumeration-safe contract means we can't 4xx the request.
    if not _state.warned_host_mismatch then
        _state.warned_host_mismatch = true
        local raw = (_state.trust_proxy and h["x-forwarded-host"]) or h.host or "(nil)"
        raw = tostring(raw):sub(1, 200):gsub("%c", "?")
        local hosts = _state.trusted_hosts
        local list = "(none configured)"
        if hosts then
            list = table.concat(hosts, ", ")
        end
        local log = require("hull.log")
        log.warn("auth-flows: origin_for refused host '"
              .. raw .. "'. trusted_hosts = ["
              .. list .. "]. URL build skipped; subsequent email "
              .. "sends to this user-flow will be silently dropped "
              .. "until the host is added. Set public_origin or "
              .. "trust_request_host = true to override (behind a "
              .. "proxy that sets X-Forwarded-Host, set trust_proxy).")
    end
    return nil
end

-- ── Cross-site guard (login CSRF) ──────────────────────────────────
-- Every POST that signs a browser in or changes the account behind its
-- session - login, the 2FA step, a magic-link or verify click, a reset, an
-- email change, its confirm and revoke - must come from the app's own pages.
-- An attacker page that auto-submits a form to /login with the ATTACKER's
-- credentials signs the victim in to the attacker's account (whatever the
-- victim then saves lands there); SameSite cookies do not stop it (a logged-
-- out victim has no cookie to withhold, and the answer's Set-Cookie is kept),
-- nor does the CSRF middleware (it passes a request with no session).
--
-- Sec-Fetch-Site, which every current browser sends, must be same-origin or
-- none: cross-site AND same-site are refused (a sibling subdomain is often
-- less trusted than the app). A browser that sends none (Safari before 16.4)
-- still sends Origin on a POST: it - or failing that Referer - must name the
-- app (public_origin, a trusted_hosts entry, or the request's own Host, which
-- a forged browser request cannot change). With no such header at all the
-- client is not a browser, and only a JSON body is taken: a cross-site form
-- cannot send that Content-Type without a CORS preflight.

-- Is the Content-Type JSON? Its essence (before any parameter) must be
-- application/json: a substring test took "text/plain; x=application/json",
-- a CORS-safelisted type any cross-site form or no-cors fetch can send.
local function is_json_ct(req)
    local ct = req.headers and req.headers["content-type"]
    if type(ct) ~= "string" then return false end
    return _text.trim(ct:match("^[^;]*")):lower() == "application/json"
end

-- "scheme://authority" of an Origin / Referer value, lower-cased; nil for
-- anything else ("null", a missing or malformed header).
local function header_origin(v)
    if type(v) ~= "string" then return nil end
    local o = v:match("^(https?://[^/?#]+)")
    return o and o:lower() or nil
end

local function origin_trusted(o, h)
    if _state.public_origin and o == header_origin(_state.public_origin) then
        return true
    end
    local authority = o:match("^https?://(.*)$")
    if _state.trusted_hosts then
        for _, allowed in ipairs(_state.trusted_hosts) do
            local a = allowed:lower()
            if authority == a or authority:match("^(.-):%d+$") == a then
                return true
            end
        end
    end
    local host, port = request_host(h)
    return host ~= nil
        and authority == (host .. (port and (":" .. port) or "")):lower()
end

-- `allow_bare`: a request with no provenance header at all is let through
-- whatever its body (logout, where forcing one only signs a user out).
local function same_origin_request(req, allow_bare)
    local h = req.headers or {}
    local site = h["sec-fetch-site"]
    if type(site) == "string" and site ~= "" then
        return site == "same-origin" or site == "none"
    end
    if h.origin ~= nil then
        local o = header_origin(h.origin)
        return o ~= nil and origin_trusted(o, h)
    end
    local r = header_origin(h.referer)
    if r then return origin_trusted(r, h) end
    if allow_bare then return true end
    if not is_json_ct(req) then return false end
    local ok, t = pcall(json.decode, req.body or "")
    return ok and type(t) == "table"
end

-- Answer 403 and return true when the request is cross-site.
local function refuse_cross_site(req, res, allow_bare)
    if same_origin_request(req, allow_bare) then return false end
    res:status(403):json({ error = "forbidden: cross-site request" })
    return true
end

local function send_email(to, template_name, ctx)
    if not email_rate_allow(to) then return end
    -- Belt-and-suspenders: scrub user.password_hash in the ctx so a
    -- caller that forgot to use strip_user_secrets still doesn't
    -- leak via a template.
    if type(ctx) == "table" and type(ctx.user) == "table" then
        ctx.user = strip_user_secrets(ctx.user)
    end
    local r = render_template(template_name, ctx)
    _state.email_send(to, r.subject, r.html, r.text)
end

-- Run fn after the response has gone. Issuing a token and sending its email
-- happen only for SOME addresses (an existing account, or a new one), and an
-- email send is a network round trip: done inline, response time said
-- whether an account exists. Deferred onto the event loop, every outcome
-- answers equally fast. Inline only where there is no loop to defer onto
-- (an in-process test harness); a failure is logged, not raised - the
-- response is already sent.
local function after_response(fn)
    local H = hull
    local run = function()
        local ok, err = pcall(fn)
        if not ok then
            require("hull.log").warn("auth-flows: deferred email failed: "
                                     .. tostring(err))
        end
    end
    if H and H.async and H.sleep then
        local spawned = pcall(H.async, function()
            pcall(H.sleep, 1)   -- yields to the loop; fails without one
            run()
        end)
        if spawned then return end
    end
    run()
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

local function bump_failed_login(user_id, max)
    local now = time.now()
    -- Portable conditional upsert. The original used INSERT ... ON CONFLICT
    -- DO UPDATE, which MySQL spells differently (ON DUPLICATE KEY UPDATE), so
    -- do it in two portable steps that hold on every backend: an atomic
    -- CASE-based UPDATE (standard SQL), then an INSERT only when no row
    -- matched (first failure). No explicit transaction, so it composes safely
    -- even if the login handler already runs inside one.
    --
    -- The CASE logic mirrors the original (Round-9 HIGH-2): when the previous
    -- lockout window has expired (locked_until IS NOT NULL AND < now) but the
    -- row hasn't been gc'd yet (gc_expired needs last_failed_at < now-86400),
    -- the stale failed_count = max-from-prior-cycle would push
    -- `failed_count + 1 >= max` straight into a fresh lockout on the next bad
    -- attempt (pre-fix: one attempt per 15-min window for the next 24h, not
    -- five). Reset to 1 when the prior window has elapsed; else increment and
    -- lock once failed_count+1 reaches the threshold.
    local update_sql =
        "UPDATE _hull_auth_login_attempts SET "
        .. "  failed_count = CASE "
        .. "    WHEN locked_until IS NOT NULL AND locked_until < ? THEN 1 "
        .. "    ELSE failed_count + 1 "
        .. "  END, "
        .. "  last_failed_at = ?, "
        .. "  locked_until = CASE "
        .. "    WHEN locked_until IS NOT NULL AND locked_until < ? THEN NULL "
        .. "    WHEN failed_count + 1 >= ? THEN ? + ? "
        .. "    ELSE NULL "
        .. "  END "
        .. "WHERE user_id = ?"
    local update_args = {
        now,           -- failed_count CASE: window-expired check
        now,           -- last_failed_at
        now,           -- locked_until CASE: window-expired check
        max, now, _state.lockout_duration,
        user_id }
    if db.exec(update_sql, update_args) > 0 then return end
    -- No existing row: first failure for this user. INSERT; if a concurrent
    -- request won the insert race (duplicate PK), fall back to the atomic
    -- UPDATE so the increment still lands.
    --
    -- The exec is wrapped in a CLOSURE, not pcall(db.exec, ...): the _hull_*
    -- namespace guard bypasses stdlib by inspecting db.exec's immediate Lua
    -- caller's chunk source for a "hull." prefix (lua_is_stdlib_caller). A bare
    -- pcall(db.exec, ...) puts pcall -- a C frame -- at that level, so the
    -- guard would deny this write to the reserved _hull_auth_login_attempts.
    local ok = pcall(function()
        db.exec(
            "INSERT INTO _hull_auth_login_attempts "
            .. "(user_id, failed_count, last_failed_at, locked_until) "
            .. "VALUES (?, 1, ?, NULL)",
            { user_id, now })
    end)
    if not ok then
        db.exec(update_sql, update_args)
    end
end

local function clear_failed_logins(user_id)
    db.exec("DELETE FROM _hull_auth_login_attempts WHERE user_id = ?",
            { user_id })
end

-- The lockout rows a login touches: one for (account, client IP), keyed
-- `<user_id> \31 <ip>` in the same column (no schema change), and the
-- account-wide one keyed by the user id alone.
local IP_SEP = "\31"
local function attempt_ip_key(uid, req)
    return tostring(uid) .. IP_SEP
           .. (_request.limit_key(_request.client_ip(req, _state.trust_proxy)) or "_anon")
end

-- Every row for an account, after a password reset proves control of it.
local function clear_all_failed_logins(uid)
    local pat = tostring(uid):gsub("[!%%_]", "!%0") .. IP_SEP .. "%"
    db.exec("DELETE FROM _hull_auth_login_attempts "
            .. "WHERE user_id = ? OR user_id LIKE ? ESCAPE '!'",
            { uid, pat })
end

-- ── Pwned-password check (opt-in) ──────────────────────────────────
--
-- Wraps hull.web.pwned with the init-time options. Returns true if
-- the password is in the breach corpus (caller should reject);
-- false otherwise (including HIBP-outage fail-open).
local function check_pwned(password)
    if not _state.check_pwned_passwords then return false end
    return pwned.check(password,
        _state.pwned_endpoint and { endpoint = _state.pwned_endpoint } or nil)
end

-- ── Sign-in event emit + finish-login helper ──────────────────────
--
-- emit_event is a no-op when sign_in_log isn't enabled; the
-- conditional lives here so call sites stay clean.
--
-- Round-8 MEDIUM-9: pcall-wrap the audit-log write so a flaky DB row
-- doesn't unwind out of password_reset_confirm /
-- email_change_confirm / email_change_revoke AFTER the password (or
-- email) has already been mutated. Pre-round-8 the bare call would
-- 500 the response and the user would retry (sending another email).
-- The JS sibling already had try/catch for the same reason.
local function emit_event(user_id, kind, req, opts)
    if _state.audit_log then
        local ok, err = pcall(_state.audit_log.record,
                              user_id, kind, req, opts)
        if not ok then
            local log = require("hull.log")
            log.warn("auth-flows: audit_log.record('" .. tostring(kind)
                  .. "') failed: " .. tostring(err))
        end
    end
end

-- Shared tail of every login path (password 1FA, magic-link, 2FA
-- verify). Hands the user off to the app-supplied on_login callback
-- (typically session.login_handler) with the factors metadata in the
-- ctx 4th arg. The audit row + new-device hook are now emitted by
-- session.login_handler - see hull/web/middleware/session.lua's
-- audit_log/on_new_device opts. That single seam covers OAuth too.
--
-- emit_event (password_reset_completed / email_change_* further
-- down) is still owned here because those are flow-completion
-- events, not login events, and only auth-flows knows about them.
local function finish_login(req, res, user, factors)
    -- Round-8 LOW-10: scrub password_hash before handing user to
    -- the app callback. session.login_handler stashes the user blob
    -- in the session payload, which is then JSON-encoded and read
    -- back on every load; a leaked hash would persist on disk + in
    -- session.list_for_user output.
    _state.on_login(req, res, strip_user_secrets(user),
                    { factors = factors })
end

-- ── Body parsing ───────────────────────────────────────────────────
-- Accept either JSON or url-encoded form. Returns the parsed table
-- or {} on parse failure (the route handler does field-presence
-- validation separately).
local function parse_body(req)
    local body = req.body or ""
    if #body == 0 then return {} end
    if is_json_ct(req) then
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
            local v = encoding.url.decode(pair:sub(eq + 1), { form = true })
            out[k] = v
        end
    end
    return out
end

-- Trivial email shape check. The actual deliverability is
-- determined by the email provider - we just guard against
-- obviously-garbage input.
local function is_email_ish(s)
    if type(s) ~= "string" then return false end
    if #s < 3 or #s > 254 then return false end
    -- Round-9 MEDIUM-8: reject any control byte (< 0x20 or 0x7f).
    -- Pre-fix, `"victim@x.com\0filler"` passed the @ + . shape check;
    -- many SMTP transports truncate at NUL so the upstream actually
    -- delivers to `victim@x.com`, while the per-recipient rate-limit
    -- key (lowercased verbatim) hashes into a different bucket per
    -- appended filler. The same trick worked for `\t`, `\r`, `\n`
    -- (header-injection territory) and `\x7f`. Tightening here closes
    -- the class at the gate.
    for i = 1, #s do
        local b = string.byte(s, i)
        if b < 0x20 or b == 0x7f then return false end
    end
    -- One address, exactly: a single '@' and none of the characters that
    -- separate or quote addresses. "victim@x.com,attacker@evil.com" passed
    -- the shape check, and an app whose email_send hands the string to an
    -- HTTP provider (which splits on commas) mailed the link to both.
    if s:find('[,;<>"()%s]') then return false end
    local at = s:find("@", 1, true)
    if not at or at == 1 or at == #s then return false end
    if s:find("@", at + 1, true) then return false end
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

-- The pending email change of @p user_id, if any, deleted: whenever the
-- password is reset or replaced (see handle_password_reset_confirm). A
-- CONFIRMED change's row stays: it is what lets the old address revoke it
-- (a session thief who resets the password through the new address must not
-- erase that).
local function drop_pending_email_change(user_id)
    db.exec("DELETE FROM _hull_auth_pending_email_changes "
            .. "WHERE user_id = ? AND confirmed_at IS NULL", { user_id })
end

-- Run the app's on_password_reset (typically session.destroy_all). Logged,
-- not swallowed: the recommended body revokes every session, so a throw means
-- a suspected-compromise cleanup did not run - an operator must see it.
local function run_on_password_reset(req, res, user)
    if not _state.on_password_reset then return end
    local ok, cb_err = pcall(_state.on_password_reset, req, res, user)
    if not ok then
        require("hull.log").warn("auth-flows: on_password_reset failed: "
                                 .. tostring(cb_err))
    end
end

-- A second factor enrolled on an account before its address was verified
-- may be the pre-registrant's: whoever registered someone else's address
-- (with require_verified_email = false they can sign in unverified) could
-- enrol TOTP and keep the recovery codes. When the mailbox holder sets the
-- password at verification (or by reset), that enrolment goes too, through
-- `totp_disable(user_id)` - typically `totp.disable`. Without the hook an
-- enrolment that exists is only logged: the app's on_password_reset must
-- then remove it.
local function drop_preverify_totp(uid)
    if _state.totp_disable then
        local ok, err = pcall(_state.totp_disable, uid)
        if not ok then
            require("hull.log").warn("auth-flows: totp_disable failed: "
                                     .. tostring(err))
        end
        return
    end
    if _state.enable_totp then
        local ok, enrolled = pcall(_state.user_totp_enrolled, uid)
        if ok and enrolled then
            require("hull.log").warn("auth-flows: account " .. tostring(uid)
                .. " has a TOTP enrolment made before its email was verified "
                .. "and no totp_disable hook is configured; remove it in "
                .. "on_password_reset (pass totp_disable = totp.disable)")
        end
    end
end

-- The mailbox holder chose @p new_hash for an account that was not verified
-- yet (a verify with new_password, or a reset). Everything a pre-registrant
-- could have attached goes first - the password, a pending email change, a
-- second factor, sessions - and the address is marked verified LAST, so a
-- failure part way never leaves a verified account with the old password.
local function replace_unverified_credentials(req, res, user, uid, new_hash)
    _state.user_set_password(uid, new_hash)
    drop_pending_email_change(uid)
    drop_preverify_totp(uid)
    clear_all_failed_logins(uid)
    run_on_password_reset(req, res, user)
    _state.user_set_email_verified(uid, true)
    user.email_verified = true
end

-- Apply the three security headers that every auth-flow HTML
-- response wants: clickjacking, cache, referrer. No opt-out because
-- there's no legitimate reason to frame your own auth flow, cache
-- it client-side, or leak the referrer when navigating off it.
-- Set BEFORE writing the body so res:html still owns content-type.
local function secure_html(res)
    res:header("X-Frame-Options", "DENY")
    res:header("Cache-Control", "no-store")
    res:header("Referrer-Policy", "strict-origin-when-cross-origin")
    return res
end

-- ── Route handlers ─────────────────────────────────────────────────

local function handle_register(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) then
        return res:status(400):json({ error = "invalid email" })
    end
    -- 256 char upper bound prevents PBKDF2 amplification DoS - a
    -- 10 MB submitted password would hash for multiple seconds at
    -- the default 600k iters. 256 covers any realistic passphrase
    -- (bcrypt's hard limit is 72 for comparison).
    if not password_len_ok(body.password) then
        return res:status(400):json({ error = "invalid password length" })
    end
    -- Pwned-password check runs BEFORE user_find_by_email so a
    -- breached password is rejected with the same error regardless
    -- of whether the email already exists - enumeration-safe.
    if check_pwned(body.password) then
        return res:status(400):json({
            error = "password appears in known data breaches; choose another",
        })
    end
    -- Enumeration-safe: returns ok whether the email exists or not.
    -- If it does exist, no email goes out (we don't want to spam
    -- existing users, and we don't want to leak existence).
    -- Hash FIRST, on both branches: PBKDF2 is by far the slowest step, and
    -- running it only for new addresses let response time tell an attacker
    -- which ones already have an account.
    local pw_hash = crypto.hash_password(body.password)
    local existing = _state.user_find_by_email(body.email)
    if existing then return generic_ok(res) end
    local user_id = _state.user_create(body.email, pw_hash)
    local user = _state.user_get(user_id)
    if not user then
        return res:status(500):json({ error = "user_create returned an id that user_get cannot resolve" })
    end

    local origin = origin_for(req)
    after_response(function()
        local token = issue_token(user_id,
            ACTIONS.verify_email, _state.verify_ttl)
        if origin then
            local verify_url = origin .. _state.prefix
                               .. "/verify?token=" .. token
            send_email(body.email, "welcome", {
                user = user, verify_url = verify_url, token = token,
            })
        end
    end)
    res:json({ ok = true })
end

-- POST /auth/verify/resend { email } - re-issue the welcome /
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
    if not user or is_verified(user) then return generic_ok(res) end
    local user_id = user_uid(user)
    local origin = origin_for(req)
    after_response(function()
        local token = issue_token(user_id, ACTIONS.verify_email,
                                   _state.verify_ttl)
        if origin then
            local verify_url = origin .. _state.prefix
                               .. "/verify?token=" .. token
            send_email(body.email, "welcome", {
                user = user, verify_url = verify_url, token = token,
            })
        end
    end)
    res:json({ ok = true })
end

-- ── Email verification ─────────────────────────────────────────────
--
-- Anyone can register any address and choose its password, so a click on
-- the welcome link proves only that the clicker reads the mailbox - not who
-- chose the password. Verification therefore takes two steps:
--
--   GET  <prefix>/verify?token=...   never consumes the token (mail scanners
--        such as Safe Links prefetch every link). It renders a small form, or
--        redirects to `verify_form_redirect?token=...` for an app-rendered one.
--   POST <prefix>/verify {token, password}      the mailbox holder also knows
--        the account's password: the address is verified, the password kept.
--        A wrong password answers 401 (offering new_password), leaves the
--        token usable, and counts toward the login lockout - so it is no
--        better a password oracle than /login.
--   POST <prefix>/verify {token, new_password}  proves the mailbox only: the
--        password is replaced, and with it everything a pre-registrant could
--        have attached (pending email change, a TOTP enrolment, sessions).
--
-- An already-verified account just consumes the token. Nothing is ever
-- voided silently; the owner always chooses.

-- Is this request a JSON API call (answer JSON) or a browser form (answer a
-- page / redirect)?
local function wants_json(req)
    return is_json_ct(req)
end

-- The default verification page. The token is a verified envelope (base64url
-- body, '.', hex tag: a fixed alphabet), so it needs no escaping; the error
-- strings are module constants. No script; the token in the body is what a
-- cross-site form cannot supply, as with the default TOTP form.
local function default_verify_form_html(token, err)
    local action = _state.prefix .. "/verify"
    return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        .. '<title>Verify your email</title></head>'
        .. '<body style="font-family:sans-serif;max-width:400px;margin:4em auto;">'
        .. '<h1>Verify your email</h1>'
        .. (err and ('<p role="alert"><strong>' .. err .. '</strong></p>') or '')
        .. '<form method="POST" action="' .. action .. '">'
        .. '<input type="hidden" name="token" value="' .. token .. '">'
        .. '<p><label>Your password: <input type="password" name="password" '
        .. 'autocomplete="current-password" required></label></p>'
        .. '<button type="submit">Verify</button></form>'
        .. '<h2>Did not choose a password, or forgot it?</h2>'
        .. '<form method="POST" action="' .. action .. '">'
        .. '<input type="hidden" name="token" value="' .. token .. '">'
        .. '<p><label>New password: <input type="password" name="new_password" '
        .. 'autocomplete="new-password" minlength="8" maxlength="256" required>'
        .. '</label></p>'
        .. '<button type="submit">Set password and verify</button></form>'
        .. '<p style="color:#666;font-size:smaller">Setting a new password '
        .. 'signs out every session of this account.</p></body></html>'
end

local function verify_fail(req, res, status, msg)
    if wants_json(req) then
        return res:status(status):json({ error = msg })
    end
    return secure_html(res):status(status):html(msg)
end

local function verify_ok(req, res)
    gc_expired()
    if wants_json(req) then
        return res:json({ ok = true, redirect = _state.verify_redirect })
    end
    return res:redirect(_state.verify_redirect, 303)
end

local function handle_verify_page(req, res)
    local token = req.query and req.query.token
    local env, err = parse_token(token, ACTIONS.verify_email)
    if not env then
        return secure_html(res):status(400):html("verification failed: " .. (err or "?"))
    end
    if token_already_used(token) then
        return secure_html(res):status(400):html("verification failed: replayed")
    end
    local user = _state.user_get(env.sub)
    if not user then
        return secure_html(res):status(400):html("verification failed")
    end
    if is_verified(user) then
        return res:redirect(_state.verify_redirect)
    end
    if _state.verify_form_redirect then
        local sep = _state.verify_form_redirect:find("?", 1, true) and "&" or "?"
        return res:redirect(_state.verify_form_redirect .. sep .. "token=" .. token)
    end
    secure_html(res):html(default_verify_form_html(token))
end

local VERIFY_WRONG_PASSWORD = "password does not match; to set a new password "
    .. "instead, submit new_password"

local function handle_verify(req, res)
    if refuse_cross_site(req, res) then return end
    local body = parse_body(req)
    local token = body.token
    local env, err = parse_token(token, ACTIONS.verify_email)
    if not env then
        return verify_fail(req, res, 400, "verification failed: " .. (err or "?"))
    end
    if token_already_used(token) then
        return verify_fail(req, res, 400, "verification failed: replayed")
    end
    local user = _state.user_get(env.sub)
    if not user then
        return verify_fail(req, res, 400, "verification failed")
    end
    local uid = user_uid(user)
    if is_verified(user) then
        mark_token_used(token, env.exp)
        return verify_ok(req, res)
    end

    if body.new_password ~= nil then
        local pw = body.new_password
        if not password_len_ok(pw) then
            return verify_fail(req, res, 400, "invalid password length")
        end
        if check_pwned(pw) then
            return verify_fail(req, res, 400,
                "password appears in known data breaches; choose another")
        end
        local new_hash = crypto.hash_password(pw)
        if not mark_token_used(token, env.exp) then
            return verify_fail(req, res, 400, "verification failed: replayed")
        end
        replace_unverified_credentials(req, res, user, uid, new_hash)
        emit_event(uid, "password_reset_completed", req,
                   { metadata = { via = "verify" } })
        return verify_ok(req, res)
    end

    local pw = body.password
    if type(pw) ~= "string" then
        return verify_fail(req, res, 400, "password or new_password required")
    end
    -- The same lockout rows as /login: a wrong password here counts there,
    -- and a locked account answers as a wrong password would, without the
    -- check - this route must not be a second, unthrottled oracle.
    local ip_key = attempt_ip_key(uid, req)
    local locked = lockout_remaining(ip_key) > 0 or lockout_remaining(uid) > 0
    local stored = not locked and password_len_ok(pw, false) and stored_password_hash(user)
    local ok = type(stored) == "string" and crypto.verify_password(pw, stored)
    if not ok then
        if not locked then
            bump_failed_login(ip_key, _state.max_failed_logins)
            bump_failed_login(uid, _state.max_failed_logins_per_account)
        end
        if wants_json(req) then
            return res:status(401):json({ error = VERIFY_WRONG_PASSWORD,
                                          new_password_allowed = true })
        end
        return secure_html(res):status(401):html(
            default_verify_form_html(token, "That password does not match. "
                .. "Try again, or set a new password below."))
    end
    if not mark_token_used(token, env.exp) then
        return verify_fail(req, res, 400, "verification failed: replayed")
    end
    clear_failed_logins(ip_key)
    clear_failed_logins(uid)
    _state.user_set_email_verified(uid, true)
    return verify_ok(req, res)
end

-- Build the minimal default HTML form rendered when a magic-link
-- click lands and 2FA is required but the app hasn't configured a
-- custom `totp_pending_redirect`. The only interpolated value is
-- the pending token (HMAC base64url + hex tag - fixed alphabet),
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
-- channel: JSON (login, a JSON magic-link POST), or for a browser
-- (`as_page`: the magic-link page's form) an HTML form or redirect.
local function start_totp_pending(req, res, user, as_page)
    local uid = user_uid(user)
    local token = issue_token(uid, ACTIONS.totp_pending,
                               _state.totp_pending_ttl)
    if not as_page then
        return res:json({
            ok = true, pending_2fa = true, totp_token = token,
        })
    end
    -- Browser form POST (magic-link page).
    if _state.totp_pending_redirect then
        local sep = _state.totp_pending_redirect:find("?", 1, true)
                    and "&" or "?"
        return res:redirect(_state.totp_pending_redirect
                             .. sep .. "token=" .. token)
    end
    secure_html(res):html(default_totp_form_html(token))
end

local function handle_login(req, res)
    -- Login CSRF: see "Cross-site guard".
    if refuse_cross_site(req, res) then return end
    local body = parse_body(req)
    -- 256 char upper bound matches register; prevents PBKDF2
    -- amplification DoS via mega-passwords. Generic error keeps
    -- enumeration-safety (over-length is just another wrong cred).
    if not is_email_ish(body.email)
       or not password_len_ok(body.password, false) then
        return res:status(400):json({ error = "invalid credentials" })
    end
    local user = _state.user_find_by_email(body.email)
    -- Lockout: when the user exists AND is currently locked, short-
    -- circuit to the SAME 401 + "invalid credentials" that the wrong-
    -- password branch returns. Round-8 HIGH-4: prior code returned
    -- 429 + Retry-After in this branch, which leaked account
    -- existence - an attacker could deliberately trip a lockout
    -- against a candidate address (5 bad guesses) and then enumerate
    -- registered emails by observing 429 vs 401. The locked state is
    -- preserved internally (the counter still ticks, the user still
    -- can't log in until the window expires) but the wire response is
    -- now indistinguishable from a wrong-password reply. Apps that
    -- want to surface "you're locked, try again in N seconds" UX to a
    -- user who's already authenticated through a different channel
    -- (e.g. mobile app) can read the counter via the (private)
    -- lockout_remaining helper on their own.
    local uid    = user and user_uid(user)
    local ip_key = uid and attempt_ip_key(uid, req)
    local pre_locked = user and (lockout_remaining(ip_key) > 0
                                 or lockout_remaining(uid) > 0)
    -- Run verify_password unconditionally when enumeration_safe is on,
    -- using a pre-computed dummy hash on the unknown-email branch so
    -- network timing is identical between known and unknown emails.
    -- See init() for the dummy hash + the threat model. Opt-out via
    -- enumeration_safe = false (test fixtures only).
    local pwhash = (user and user.password_hash) or _state._dummy_pwhash
    local pw_ok  = (_state.enumeration_safe or user ~= nil)
                   and crypto.verify_password(body.password, pwhash)
    if pre_locked or not user or not user.password_hash or not pw_ok then
        if user and not pre_locked then
            bump_failed_login(ip_key, _state.max_failed_logins)
            bump_failed_login(uid, _state.max_failed_logins_per_account)
        end
        return res:status(401):json({ error = "invalid credentials" })
    end
    if _state.require_verified_email and not is_verified(user) then
        return res:status(403):json({ error = "email not verified" })
    end
    -- Successful auth - clear this address's row and the account-wide one
    -- so subsequent typos don't accumulate against a long-standing baseline.
    clear_failed_logins(ip_key)
    clear_failed_logins(uid)
    if _state.enable_totp
       and _state.user_totp_enrolled(user_uid(user)) then
        return start_totp_pending(req, res, user)
    end
    finish_login(req, res, user, "password")
end

local function handle_logout(req, res)
    -- We don't know the user_id here without inspecting the
    -- session - that's the app's responsibility. Apps that want
    -- a "logout" event in the audit log can call audit_log.record
    -- inside their on_logout callback.
    -- A cross-site POST (an attacker page auto-submitting a form) is
    -- refused, as oauth's logout does: SameSite=Lax keeps the session cookie
    -- off it, but the clearing Set-Cookie in the answer would still sign the
    -- victim out. A client that sends no provenance header at all is let
    -- through: forcing a logout is all a forged one could do.
    if refuse_cross_site(req, res, true) then return end
    if _state.on_logout then
        return _state.on_logout(req, res)
    end
    res:redirect("/")
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
        -- Guard the create->get race / adapter inconsistency: a nil user here
        -- would issue a magic-link token with sub=nil and then error in
        -- send_email(user...). Stay enumeration-safe (matches the guarded
        -- sibling sites in handle_register / handle_magic_link_consume).
        if not user then return generic_ok(res) end
    end
    local origin = origin_for(req)
    after_response(function()
        local token = issue_token(user_uid(user),
            ACTIONS.magic_link, _state.magic_link_ttl,
            { eb = email_binding(user) })
        if origin then
            local link = origin .. _state.prefix
                         .. "/magic-link/consume?token=" .. token
            send_email(body.email, "magic_link", {
                user = user, link = link, token = token,
            })
        end
    end)
    res:json({ ok = true })
end

-- ── Single-use links: GET shows, POST consumes ─────────────────────
-- A mailed single-use link (magic link, email-change confirm and revoke)
-- is not consumed by its GET: mail scanners prefetch links, and a
-- prefetch signed the scanner in (the user's own click then answered
-- "replayed"), confirmed a change nobody read, or cancelled it. The GET
-- checks the token without using it and answers a page whose form POSTs
-- it back; the POST consumes it - as the verify flow does. A JSON client
-- POSTs {token} itself.

-- The token is a verified envelope (fixed alphabet) and the strings are
-- module constants, so nothing needs escaping. No script.
local function link_form_html(path, token, title, button)
    return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        .. '<title>' .. title .. '</title></head>'
        .. '<body style="font-family:sans-serif;max-width:400px;margin:4em auto;">'
        .. '<h1>' .. title .. '</h1>'
        .. '<form method="POST" action="' .. _state.prefix .. path .. '">'
        .. '<input type="hidden" name="token" value="' .. token .. '">'
        .. '<button type="submit">' .. button .. '</button></form></body></html>'
end

-- The GET side: the envelope and its single use, nothing consumed.
-- Returns token, env - or nil after answering 400.
local function link_page_token(req, res, action, fail)
    local token = req.query and req.query.token
    local env, err = parse_token(token, action)
    if not env then
        secure_html(res):status(400):html(fail .. ": " .. (err or "?"))
        return nil
    end
    if token_already_used(token) then
        secure_html(res):status(400):html(fail .. ": replayed")
        return nil
    end
    return token, env
end

local function handle_magic_link_page(req, res)
    local token = link_page_token(req, res, ACTIONS.magic_link, "magic link failed")
    if not token then return end
    secure_html(res):html(link_form_html("/magic-link/consume", token,
        "Sign in", "Sign in"))
end

local function handle_magic_link_consume(req, res)
    -- A cross-site form would sign the victim in to the attacker's account
    -- with the attacker's own link (login CSRF).
    if refuse_cross_site(req, res) then return end
    local token = parse_body(req).token
    local env, err = consume_token(token, ACTIONS.magic_link)
    if not env then
        return verify_fail(req, res, 400, "magic link failed: " .. (err or "?"))
    end
    local user = _state.user_get(env.sub)
    -- A magic link is bound to the address it was sent to: after an email
    -- change, one still sitting in the old mailbox no longer signs in.
    if not user or env.eb ~= email_binding(user) then
        return verify_fail(req, res, 400, "magic link failed")
    end
    -- Magic-link clicks count as proof of email ownership. An account that
    -- was not verified yet and HAS a password may carry one somebody else
    -- chose (anyone can register any address): signing in here would hand
    -- that somebody the owner's account, and silently replacing it would
    -- lock out an owner who chose it. So the click goes through the verify
    -- step instead - confirm that password or set a new one (handle_verify).
    -- A passwordless account (magic_link_auto_signup) has nothing to keep.
    -- An account whose hash cannot be read is treated as having one.
    if not is_verified(user) then
        local uid = user_uid(user)
        if stored_password_hash(user) ~= nil then
            local vtok = issue_token(uid, ACTIONS.verify_email, _state.verify_ttl)
            gc_expired()
            if _state.verify_form_redirect then
                local sep = _state.verify_form_redirect:find("?", 1, true) and "&" or "?"
                return res:redirect(_state.verify_form_redirect .. sep .. "token=" .. vtok)
            end
            return secure_html(res):html(default_verify_form_html(vtok))
        end
        _state.user_set_email_verified(uid, true)
        user.email_verified = true
    end
    gc_expired()
    if _state.enable_totp
       and _state.user_totp_enrolled(user_uid(user)) then
        return start_totp_pending(req, res, user, not wants_json(req))
    end
    finish_login(req, res, user, "magic_link")
end

-- POST /auth/totp-verify { token, code } - second factor.
-- The pending token is NOT consumed on a failed code attempt
-- (apps must rate-limit this route to bound retry; see the
-- module header for the recommended ratelimit.middleware
-- snippet). On success it's burned exactly like a single-use
-- token, then on_login runs.
local function handle_totp_verify(req, res)
    if not _state.enable_totp then
        return res:status(404):json({ error = "totp not enabled" })
    end
    -- Login CSRF with the attacker's own pending token and code.
    if refuse_cross_site(req, res) then return end
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
    -- Round-9 HIGH-4: pass `req` through so the totp_verify callback
    -- can extract the remote IP and gate per-IP attempts in addition
    -- to per-user. The default totp.verify accepts the 3rd arg;
    -- custom callbacks that ignore it keep round-8 behaviour.
    local ok = _state.totp_verify(user, body.code, req)
    if not ok then
        return res:status(401):json({ error = "invalid code" })
    end
    -- Round-8 MEDIUM-6: the prior code discarded mark_token_used's
    -- return value, so two concurrent POSTs of the same {token, code}
    -- both passed the token_already_used probe, both verified, both
    -- called finish_login → two sessions minted from one pending-2FA
    -- token. The mark IS atomic at the DB layer (INSERT OR IGNORE
    -- on the used-tokens table); we just have to ACT on its return.
    -- If we lost the race, the OTHER concurrent request will mint
    -- the session; we bail with the same shape as a stale-token reply.
    if not mark_token_used(body.token, env.exp) then
        return res:status(400):json({ error = "totp token already used" })
    end
    gc_expired()
    -- 2FA path - record both factors. Apps reading audit logs
    -- can use this to distinguish "password-only" from "with
    -- 2FA" logins for compliance reporting.
    finish_login(req, res, user, "password+totp")
end

local function handle_password_reset_request(req, res)
    local body = parse_body(req)
    if not is_email_ish(body.email) then
        return res:status(400):json({ error = "invalid email" })
    end
    local user = _state.user_find_by_email(body.email)
    if not user then return generic_ok(res) end
    local origin = origin_for(req)
    after_response(function()
        local token = issue_token(user_uid(user),
            ACTIONS.password_reset, _state.reset_ttl, reset_token_extra(user))
        if origin then
            local link = origin .. _state.prefix
                         .. "/password-reset/confirm?token=" .. token
            send_email(body.email, "password_reset", {
                user = user, link = link, token = token,
            })
        end
    end)
    res:json({ ok = true })
end

local function handle_password_reset_confirm(req, res)
    if refuse_cross_site(req, res) then return end
    local body = parse_body(req)
    -- Same upper bound as handle_register; see comment there.
    if not password_len_ok(body.password) then
        return res:status(400):json({ error = "invalid password length" })
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
    if not user or not reset_binding_holds(env, user) then
        return res:status(400):json({ error = "reset failed" })
    end
    local new_hash = crypto.hash_password(body.password)
    if not is_verified(user) then
        -- The reset link proves the mailbox and its holder chose this
        -- password: the account is verified, as a verify with new_password
        -- does - and loses what a pre-registrant could have attached.
        replace_unverified_credentials(req, res, user, env.sub, new_hash)
        emit_event(env.sub, "password_reset_completed", req)
        gc_expired()
        return res:json({ ok = true })
    end
    _state.user_set_password(env.sub, new_hash)
    -- A pending email change goes with the old password: started from a
    -- hijacked session, its confirm link otherwise still moved the account
    -- to the attacker's address after the owner reset the password.
    drop_pending_email_change(env.sub)
    -- A successful reset also unlocks the account: the user
    -- demonstrably controls the email, so any prior lockout is
    -- moot. (If they don't reset, the lockout window expires
    -- naturally per lockout_duration.)
    clear_all_failed_logins(env.sub)
    -- Audit + give the app a chance to invalidate existing
    -- sessions. The recommended on_password_reset implementation
    -- is `function(req,res,user) session.destroy_all(user.id) end`;
    -- apps that want to keep the current session can filter it
    -- out via session.destroy_others instead.
    emit_event(env.sub, "password_reset_completed", req)
    run_on_password_reset(req, res, user)
    gc_expired()
    res:json({ ok = true })
end

local function handle_email_change(req, res)
    -- This route assumes the app has authenticated the request
    -- (e.g. via auth.session_middleware) and stashed the user id
    -- on req.ctx.user_id. The module doesn't depend on a specific
    -- session shape - apps wire this in.
    local user_id = req.ctx and req.ctx.user_id
    if not user_id then
        return res:status(401):json({ error = "not authenticated" })
    end
    if refuse_cross_site(req, res) then return end
    local body = parse_body(req)
    if not is_email_ish(body.new_email) then
        return res:status(400):json({ error = "invalid email" })
    end
    -- A session is not enough: whoever stole one would move the account to
    -- an address they read, confirm it in seconds, and own the account for
    -- good through a password reset there. The current password (counted
    -- toward the login lockout, like /login) proves the account holder; an
    -- app with its own proof of a recent sign-in - or passwordless accounts -
    -- passes email_change_reauth(req, user) -> true instead.
    local current = _state.user_get(user_id)
    if not current then
        return res:status(401):json({ error = "not authenticated" })
    end
    local reauthed = false
    if _state.email_change_reauth then
        local ok, r = pcall(_state.email_change_reauth, req, current)
        reauthed = ok and r == true
    end
    if not reauthed then
        local pw = body.password
        if type(pw) ~= "string" or pw == "" then
            return res:status(401):json({ error = "current password required" })
        end
        -- The lockout rows /login uses: keyed by the account's own id.
        local uid = user_uid(current) or user_id
        local ip_key = attempt_ip_key(uid, req)
        local locked = lockout_remaining(ip_key) > 0 or lockout_remaining(uid) > 0
        local stored = not locked and password_len_ok(pw, false)
                       and stored_password_hash(current)
        if not (type(stored) == "string" and crypto.verify_password(pw, stored)) then
            if not locked then
                bump_failed_login(ip_key, _state.max_failed_logins)
                bump_failed_login(uid, _state.max_failed_logins_per_account)
            end
            return res:status(401):json({ error = "invalid credentials" })
        end
    end
    -- Reject if the target email is already taken - reveals
    -- existence, but that's a UX call (the alternative is a silent
    -- accept that confuses the user).
    if _state.user_find_by_email(body.new_email) then
        return res:status(409):json({ error = "email already in use" })
    end

    -- Round-10 HIGH-1: compute the origin FIRST. Pre-fix the
    -- db.upsert ran before the origin check, so a request with a
    -- hostile Host header (off-allowlist) clobbered the victim's
    -- prior pending email-change row even though no link was ever
    -- mailed. Bailing here preserves the {ok:true} enumeration-
    -- safe shape AND leaves the DB untouched.
    local origin = origin_for(req)
    if not origin then
        return res:json({ ok = true })
    end

    -- Round-11 MEDIUM-9: reject when a pending email-change for
    -- this user already exists. Pre-fix the upsert silently
    -- destroyed the prior pending row - when the user (or attacker)
    -- clicked a stale link, email_change_confirm rejected at the
    -- `new_email` mismatch guard, killing the legitimate flow with
    -- a generic error and no retry path. Force the user to either
    -- click revoke or wait email_change_ttl. Expired pending rows
    -- are reaped by gc_expired so the user isn't blocked forever
    -- if they abandoned the prior attempt.
    -- A confirmed change keeps its row until its revoke link expires, and
    -- blocks a new one meanwhile: a thief must not bury it under a second
    -- change (whose revoke would restore only the thief's address).
    local existing = db.query(
        "SELECT new_email, confirmed_at FROM _hull_auth_pending_email_changes "
        .. "WHERE user_id = ? AND expires_at > ? LIMIT 1",
        { user_id, time.now() })
    if existing and #existing > 0 then
        if existing[1].confirmed_at ~= nil then
            return res:status(409):json({
                error = "a recent email change can still be revoked; try again later",
            })
        end
        return res:status(409):json({
            error = "pending email change exists",
            new_email = existing[1].new_email,
        })
    end

    local now = time.now()
    local token = issue_token(user_id, ACTIONS.email_change,
        _state.email_change_ttl, { new_email = body.new_email })
    local token_hash = encoding.hex.encode(crypto.sha256(token))
    -- An expired row (not reaped yet) is replaced whole: an upsert kept its
    -- old_email / confirmed_at.
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            { user_id })
    db.exec("INSERT INTO _hull_auth_pending_email_changes "
            .. "(user_id, new_email, token_hash, created_at, expires_at) "
            .. "VALUES (?, ?, ?, ?, ?)",
            { user_id, body.new_email, token_hash, now,
              now + _state.email_change_ttl })

    local user = current
    local link = origin .. _state.prefix
                 .. "/email-change/confirm?token=" .. token
    -- Send to the NEW address - proves the user controls it.
    send_email(body.new_email, "email_change", {
        user = user, link = link, token = token,
        new_email = body.new_email,
    })
    -- Defense in depth: notify the OLD address with a revoke link
    -- so a stolen session cookie can't quietly move the account.
    -- Opt-in by providing templates.email_change_notify; apps
    -- without the template keep the v1 behavior. Guard `user` (nil only on a
    -- pathological row-deleted-mid-request race): the confirm email above uses
    -- a nil-safe template ctx, but user.email below is a hard deref.
    if user and _state.templates.email_change_notify then
        -- Bound to THIS change (`ch`, its confirm token's hash): a revoke
        -- link from an earlier change, in a mailbox the account has since
        -- left, does not cancel or undo a later one.
        local revoke_tok = issue_token(user_id,
            ACTIONS.email_change_revoke, _state.email_change_ttl,
            { ch = token_hash })
        local revoke_url = origin .. _state.prefix
            .. "/email-change/revoke?token=" .. revoke_tok
        send_email(user.email, "email_change_notify", {
            user = user, revoke_url = revoke_url,
            revoke_token = revoke_tok,
            new_email = body.new_email,
        })
    end
    res:json({ ok = true })
end

-- GET /auth/email-change/revoke?token=... - the page; POST consumes
-- it (see "Single-use links"). The OLD-address holder undoes the email
-- change the link was sent for: a pending one is cancelled (its row
-- deleted, so its confirm link stops working); one already confirmed is
-- reversed - the old address restored - for as long as the link lives
-- (email_change_ttl from the request). Either way every session of the
-- account is revoked through on_password_reset: the change may have come
-- from a stolen one.
local function handle_email_change_revoke_page(req, res)
    local token = link_page_token(req, res, ACTIONS.email_change_revoke, "revoke failed")
    if not token then return end
    secure_html(res):html(link_form_html("/email-change/revoke", token,
        "Undo the email change", "Undo the change"))
end

local function handle_email_change_revoke(req, res)
    if refuse_cross_site(req, res) then return end
    local env, err = consume_token(parse_body(req).token, ACTIONS.email_change_revoke)
    if not env then
        return verify_fail(req, res, 400, "revoke failed: " .. (err or "?"))
    end
    local rows = db.query(
        "SELECT token_hash, old_email, confirmed_at FROM _hull_auth_pending_email_changes "
        .. "WHERE user_id = ?", { env.sub })
    local row = rows and rows[1]
    if not row or type(env.ch) ~= "string" or type(row.token_hash) ~= "string"
       or not crypto.constant_time_eq(row.token_hash, env.ch) then
        return verify_fail(req, res, 400, "revoke failed")
    end
    local user = _state.user_get(env.sub)
    if not user then return verify_fail(req, res, 400, "revoke failed") end
    local restored = false
    if row.confirmed_at ~= nil then
        local old = row.old_email
        local holder = type(old) == "string" and _state.user_find_by_email(old)
        if type(old) ~= "string"
           or (holder and tostring(user_uid(holder)) ~= tostring(env.sub)) then
            -- Taken since by another account: nothing to restore to.
            require("hull.log").warn("auth-flows: email change of account "
                .. tostring(env.sub) .. " cannot be reverted: its previous "
                .. "address is in use; sessions revoked")
            run_on_password_reset(req, res, user)
            return verify_fail(req, res, 409, "revoke failed: the previous address is in use")
        end
        _state.user_set_email(env.sub, old)
        -- The revoke link reached the old mailbox: it is proven again.
        _state.user_set_email_verified(env.sub, true)
        user.email = old
        restored = true
    end
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            { env.sub })
    run_on_password_reset(req, res, user)
    emit_event(env.sub, "email_change_revoked", req,
               { metadata = { by = "old_address", restored = restored } })
    gc_expired()
    if wants_json(req) then return res:json({ ok = true, restored = restored }) end
    secure_html(res):html(restored
        and "Email change undone: your previous address is restored."
        or "Email change canceled.")
end

-- Does the pending change still match this confirm link? There must be a
-- pending (unconfirmed) row, for the envelope's new_email, and its
-- token_hash must be THIS token's: only the latest link of the pending
-- change confirms it, and none once the row is gone (revoked, superseded)
-- or confirmed.
local function pending_change_matches(env, token)
    local rows = db.query(
        "SELECT new_email, token_hash, confirmed_at FROM _hull_auth_pending_email_changes "
        .. "WHERE user_id = ?", { env.sub })
    local th = encoding.hex.encode(crypto.sha256(token))
    return rows ~= nil and #rows > 0
       and rows[1].confirmed_at == nil
       and rows[1].new_email == env.new_email
       and type(rows[1].token_hash) == "string"
       and crypto.constant_time_eq(rows[1].token_hash, th)
end

local function handle_email_change_page(req, res)
    local token, env = link_page_token(req, res, ACTIONS.email_change, "email change failed")
    if not token then return end
    if not pending_change_matches(env, token) then
        return secure_html(res):status(400):html("email change failed")
    end
    secure_html(res):html(link_form_html("/email-change/confirm", token,
        "Confirm your new email address", "Confirm"))
end

local function handle_email_change_confirm(req, res)
    if refuse_cross_site(req, res) then return end
    local token = parse_body(req).token
    local env, err = consume_token(token, ACTIONS.email_change)
    if not env then
        return verify_fail(req, res, 400, "email change failed: " .. (err or "?"))
    end
    local user = _state.user_get(env.sub)
    if not user or not pending_change_matches(env, token) then
        return verify_fail(req, res, 400, "email change failed")
    end
    local old_email = user.email
    _state.user_set_email(env.sub, env.new_email)
    _state.user_set_email_verified(env.sub, true)
    -- With a revoke link out (email_change_notify), the row stays, confirmed
    -- and holding the old address, until that link expires: the old address
    -- can still undo the change. Without one there is nothing to undo it with.
    if _state.templates.email_change_notify then
        db.exec("UPDATE _hull_auth_pending_email_changes "
                .. "SET confirmed_at = ?, old_email = ? WHERE user_id = ?",
                { time.now(), old_email, env.sub })
    else
        db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
                { env.sub })
    end
    emit_event(env.sub, "email_changed", req,
               { metadata = { old_email = old_email,
                              new_email = env.new_email } })
    verify_ok(req, res)
end

-- Route registration helper.
local function register_routes(app)
    local p = _state.prefix
    -- Opt-in per-IP rate limit on /login + /password-reset/request +
    -- /magic-link. Installed BEFORE the handler so abusive traffic is
    -- rejected before any DB or PBKDF2 work. Per-IP key derived from
    -- x-forwarded-for then req.remote_addr; falls back to the literal
    -- "_anon" so a malformed request can't bypass the bucket entirely.
    if _state.login_ratelimit then
        local rl_opts = type(_state.login_ratelimit) == "table"
            and _state.login_ratelimit or {}
        local mw = ratelimit.middleware({
            limit  = rl_opts.limit  or 20,
            window = rl_opts.window or 300,  -- 5 min
            -- Per-IP key via the shared hull.web._request helper
            -- (trust_proxy -> the last XFF entry, the peer our proxy saw
            -- -> remote_addr). The client writes every entry to the left
            -- of it, so keying on those (or the whole chain) let a client
            -- mint a new bucket per request. Falls back to the literal
            -- "_anon" so a malformed request can't escape the bucket
            -- entirely. App-supplied opts.key still wins.
            key    = rl_opts.key or function(req)
                return _request.limit_key(_request.client_ip(req, _state.trust_proxy)) or "_anon"
            end,
        })
        app.use("POST", p .. "/register", mw)
        app.use("POST", p .. "/login", mw)
        app.use("POST", p .. "/magic-link", mw)
        app.use("POST", p .. "/password-reset/request", mw)
    end

    app.post(p .. "/register",                 handle_register)
    app.get (p .. "/verify",                   handle_verify_page)
    app.post(p .. "/verify",                   handle_verify)
    app.post(p .. "/verify/resend",            handle_verify_resend)
    app.post(p .. "/login",                    handle_login)
    app.post(p .. "/logout",                   handle_logout)
    app.post(p .. "/magic-link",               handle_magic_link)
    app.get (p .. "/magic-link/consume",       handle_magic_link_page)
    app.post(p .. "/magic-link/consume",       handle_magic_link_consume)
    app.post(p .. "/password-reset/request",   handle_password_reset_request)
    app.post(p .. "/password-reset/confirm",   handle_password_reset_confirm)
    app.post(p .. "/email-change",             handle_email_change)
    app.get (p .. "/email-change/confirm",     handle_email_change_page)
    app.post(p .. "/email-change/confirm",     handle_email_change_confirm)
    app.get (p .. "/email-change/revoke",      handle_email_change_revoke_page)
    app.post(p .. "/email-change/revoke",      handle_email_change_revoke)
    -- Always registered so the route doesn't 404 with a confusing
    -- "no such route" when an app forgets enable_totp; the handler
    -- itself returns 404 with a clear error in that case.
    app.post(p .. "/totp-verify",              handle_totp_verify)
end

-- ── Public API ─────────────────────────────────────────────────────

--- Initialize the module. Must be called once at app startup.
-- @tparam table opts See module header for the full option list.
--- Build a turnkey adapter for the 6 `user_*` callbacks against a
-- "standard" users-table schema. Apps with a vanilla schema can
-- pass the result as `opts.users` to M.init and skip ~30 lines of
-- thin DB wrappers. Apps with a custom schema either override
-- single callbacks (opts.user_create wins over opts.users.create)
-- or skip the adapter entirely.
--
-- The adapter is **DB-backend-agnostic** - it issues standard
-- INSERT / UPDATE / SELECT against the `users` table via the
-- `db` module, no SQLite-specific syntax. Works on whatever
-- backend `hull/db` is wired to (SQLite today, Postgres planned).
--
-- Default schema assumed (portable across SQLite + Postgres):
--   CREATE TABLE users (
--       id            TEXT PRIMARY KEY,
--       email         TEXT NOT NULL UNIQUE,
--       password_hash TEXT,
--       email_verified INTEGER NOT NULL DEFAULT 0,
--       created_at    INTEGER NOT NULL,
--       updated_at    INTEGER NOT NULL
--   )
--
-- @tparam ?table opts
--   * `table`   - table name (default `"users"`).
--   * `id_gen`  - `function() -> string` for new ids (default:
--                 32 hex chars from crypto.random(16)).
-- @treturn table  Six fields: find_by_email, get, create,
--                 set_password, set_email, set_email_verified.
function M.standard_users(opts)
    opts = opts or {}
    -- Quote the app-supplied table name for the connection's dialect so a
    -- reserved word (e.g. "user", "order") or a future MySQL backend (backtick)
    -- is safe. The default connection is open by the time this runs, so its
    -- backend dialect is known.
    local name = opts.table or "users"
    -- The adapter runs on the stdlib's internal connection with stdlib
    -- identity, which the _hull_* namespace guard lets through: a _hull_
    -- table name here would read and write the module tables themselves.
    if type(name) ~= "string" or name == "" or name:lower():find("^_hull_") then
        error("auth-flows.standard_users: table must be a non-empty name "
              .. "outside the reserved _hull_ namespace")
    end
    local tbl    = db.quote_identifier(name)
    local id_gen = opts.id_gen or function()
        return crypto.random_token(16, "hex")
    end

    local function row(r)
        if not r then return nil end
        return {
            id             = r.id,
            email          = r.email,
            password_hash  = r.password_hash,
            email_verified = r.email_verified == 1,
        }
    end

    return {
        find_by_email = function(email)
            local rows = db.query(
                "SELECT * FROM " .. tbl .. " WHERE email = ?", { email })
            return rows and rows[1] and row(rows[1]) or nil
        end,
        get = function(id)
            local rows = db.query(
                "SELECT * FROM " .. tbl .. " WHERE id = ?", { id })
            return rows and rows[1] and row(rows[1]) or nil
        end,
        create = function(email, pwhash)
            local id = id_gen()
            local now = time.now()
            db.exec(
                "INSERT INTO " .. tbl
                .. " (id, email, password_hash, email_verified, "
                .. "  created_at, updated_at) "
                .. "VALUES (?, ?, ?, 0, ?, ?)",
                { id, email, pwhash, now, now })
            return id
        end,
        set_password = function(id, pwhash)
            db.exec(
                "UPDATE " .. tbl
                .. " SET password_hash = ?, updated_at = ? WHERE id = ?",
                { pwhash, time.now(), id })
        end,
        set_email = function(id, email)
            db.exec(
                "UPDATE " .. tbl
                .. " SET email = ?, updated_at = ? WHERE id = ?",
                { email, time.now(), id })
        end,
        set_email_verified = function(id, verified)
            db.exec(
                "UPDATE " .. tbl
                .. " SET email_verified = ?, updated_at = ? WHERE id = ?",
                { verified and 1 or 0, time.now(), id })
        end,
    }
end

function M.init(opts)
    opts = opts or {}
    -- Canonical `secret`; back-compat alias `state_secret` (same HMAC key).
    local secret = opts.secret or opts.state_secret
    if type(secret) ~= "string" or #secret < 32 then
        error("auth-flows.init: secret must be a string >= 32 bytes")
    end
    if type(opts.email_send) ~= "function" then
        error("auth-flows.init: email_send(to, subject, html, text) required")
    end
    if type(opts.templates) ~= "table" then
        error("auth-flows.init: templates table required")
    end
    -- Unverified accounts can sign in, so a pre-registrant (anyone can
    -- register any address) can hold a session when the mailbox holder sets
    -- the password at verification. Sessions are the app's: on_password_reset
    -- is the only place they can be revoked, so it is required here.
    -- (Checked before any state changes, so a refused init leaves none.)
    local rve = opts.require_verified_email
    if rve == nil then rve = _state.require_verified_email end
    if not rve and type(opts.on_password_reset) ~= "function" then
        error("auth-flows.init: require_verified_email = false needs "
            .. "on_password_reset (e.g. function(req, res, user) "
            .. "session.destroy_all(user.id) end): it is what revokes a "
            .. "pre-registrant's sessions when the address owner sets the "
            .. "password; pass a no-op function if the app keeps no sessions")
    end
    -- Round-9 HIGH-1: require ONE of public_origin / trusted_hosts.
    -- See _state.public_origin docstring for the threat model.
    local has_origin = type(opts.public_origin) == "string"
                       and #opts.public_origin > 0
    local has_hosts = type(opts.trusted_hosts) == "table"
                      and #opts.trusted_hosts > 0
    local trust_request = opts.trust_request_host == true
    if not has_origin and not has_hosts and not trust_request then
        error("auth-flows.init: pass `public_origin = \"https://app.example."
              .. "com\"` OR `trusted_hosts = {\"app.example.com\", ...}` "
              .. "OR `trust_request_host = true` (dev/test only). "
              .. "Click-through URLs (verify / magic-link / password-reset "
              .. "/ email-change) are built from this; without it, "
              .. "req.headers.host is attacker-controlled and a hostile "
              .. "Host header reroutes the link to a phishing origin.")
    end
    if trust_request and not (has_origin or has_hosts) then
        local log = require("hull.log")
        log.warn("auth-flows: trust_request_host = true - falling back to "
              .. "req.headers.host for URL construction. Vulnerable to "
              .. "host-header injection; use public_origin / trusted_hosts "
              .. "in production.")
    end
    if has_origin then
        -- Reject relative / scheme-less URLs early.
        if not (opts.public_origin:find("^https?://") ) then
            error("auth-flows.init: public_origin must start with "
                  .. "http:// or https://")
        end
        -- Strip trailing slash so origin .. prefix .. ... composes cleanly.
        if opts.public_origin:sub(-1) == "/" then
            opts.public_origin = opts.public_origin:sub(1, -2)
        end
    end
    if has_hosts then
        -- Each entry is a host or "host:port", the shapes origin_for
        -- can match (anything else would silently never match, and the
        -- deployment send zero emails). IPv6 literals are bracketed
        -- ([::1], [::1]:8443).
        for _, h in ipairs(opts.trusted_hosts) do
            if type(h) ~= "string" or h == "" then
                error("auth-flows.init: trusted_hosts entries must be "
                      .. "non-empty strings (got " .. type(h) .. ")")
            end
            local hh, hp = h:match("^(%[[%x:%.]+%])(.*)$")
            if not hh then hh, hp = h:match("^([%w%.%-]+)(.*)$") end
            local pn = hp and hp:match("^:(%d%d?%d?%d?%d?)$")
            if not hh or (hp ~= "" and not (pn and tonumber(pn) <= 65535)) then
                error("auth-flows.init: trusted_hosts entry '" .. h
                      .. "' must be a host name or \"host:port\" (IPv6 "
                      .. "literals bracketed, e.g. \"[::1]\"); emailed "
                      .. "links carry only the port an entry names.")
            end
        end
    end
    -- Required user-storage callbacks. Collected up front so the
    -- error message names them all rather than failing on the
    -- first missing one at request time.
    local required_user = {
        "user_find_by_email", "user_get", "user_create",
        "user_set_password", "user_set_email",
        "user_set_email_verified",
    }
    -- `opts.users` (typically from M.standard_users(...)) is a
    -- bulk adapter - its 6 functions become the defaults;
    -- explicit opts.user_X overrides still win. Keeps the
    -- orthogonality of letting apps mix-and-match (e.g. swap
    -- user_create only).
    if type(opts.users) == "table" then
        for _, k in ipairs(required_user) do
            local short = k:sub(6)  -- strip "user_" prefix
            if opts[k] == nil and type(opts.users[short]) == "function" then
                opts[k] = opts.users[short]
            end
        end
    end
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

    -- The secret is bytes. The same bytes derive the same key in Lua and JS
    -- (hull.encoding is byte-identical), but the same TEXT may not: a Lua
    -- string holds text as UTF-8, a JS byte string one character per byte.
    -- For a non-ASCII secret written as text, pass JS
    -- encoding.utf8.encode(secret) so both runtimes see the same bytes.
    _state.state_secret = secret
    _state.email_send       = opts.email_send
    _state.public_origin    = opts.public_origin
    _state.trusted_hosts    = opts.trusted_hosts
    _state.trust_request_host = opts.trust_request_host == true
    _state.trust_proxy = opts.trust_proxy == true
    -- Round-12 MEDIUM-1: reset the one-shot host-mismatch warn so a
    -- hot-reload that fixes / changes the allowlist gets a fresh
    -- diagnostic on the next bad host. Without this, the warn fires
    -- only once per process - an operator who "fixes" the config
    -- but introduces a new typo wouldn't see the second warn.
    _state.warned_host_mismatch = false
    -- Round-9 MEDIUM-6: optional user_sanitize callback. See
    -- strip_user_secrets for the threat model.
    if opts.user_sanitize ~= nil
       and type(opts.user_sanitize) ~= "function" then
        error("auth-flows.init: user_sanitize must be a function "
              .. "(user) -> safe_user")
    end
    _state.user_sanitize    = opts.user_sanitize
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
    _state.max_failed_logins_per_account = opts.max_failed_logins_per_account
                                     or _state.max_failed_logins_per_account
    _state.lockout_duration        = opts.lockout_duration
                                     or _state.lockout_duration
    _state.check_pwned_passwords   = opts.check_pwned_passwords == true
    _state.pwned_endpoint          = opts.pwned_endpoint
    _state.sign_in_log             = opts.sign_in_log == true
    _state.on_password_reset       = opts.on_password_reset
    _state.login_ratelimit         = opts.login_ratelimit
    -- email_rate_limit: opts.email_rate_limit may be false (disabled),
    -- a table { limit, window }, or nil (keep default). Reset the
    -- per-process sliding-window state so re-init in tests starts
    -- with a clean bucket pool.
    if opts.email_rate_limit ~= nil then
        _state.email_rate_limit = opts.email_rate_limit
    end
    _email_rl = {}
    _email_rl_count = 0
    -- audit-log is a top-level require now (it's a hard dep of
    -- hull/web/auth-flows in the module registry). The sign_in_log
    -- knob still gates whether we *emit* flow-completion events.
    _state.audit_log = _state.sign_in_log and audit_log or nil

    -- Timing-safe email enumeration defense. crypto.verify_password
    -- (PBKDF2-SHA256, 600k iters by default) takes 50–200ms; a 401
    -- that skipped the verify because the email was unknown would
    -- return ~instantly, letting an attacker enumerate registered
    -- emails over the network by timing. Pre-compute a dummy hash
    -- of a fixed sentinel so handle_login can run verify_password
    -- against it on the unknown-email branch and pay the same cost.
    -- The hash is computed once per process and reused per request
    -- AND per re-init (test fixtures call init() many times; each
    -- PBKDF2 was costing 50-200ms of boot time before this cache).
    if not _state._dummy_pwhash then
        _state._dummy_pwhash = crypto.hash_password(
            "auth-flows-dummy-sentinel-never-matches-real-password")
    end
    _state.verify_ttl       = opts.verify_ttl       or _state.verify_ttl
    _state.reset_ttl        = opts.reset_ttl        or _state.reset_ttl
    _state.magic_link_ttl   = opts.magic_link_ttl   or _state.magic_link_ttl
    _state.email_change_ttl = opts.email_change_ttl or _state.email_change_ttl
    _state.prefix           = opts.prefix           or _state.prefix
    _state.verify_redirect  = opts.verify_redirect  or _state.verify_redirect
    if opts.verify_form_redirect ~= nil
       and type(opts.verify_form_redirect) ~= "string" then
        error("auth-flows.init: verify_form_redirect must be a path string")
    end
    _state.verify_form_redirect = opts.verify_form_redirect
    if opts.totp_disable ~= nil and type(opts.totp_disable) ~= "function" then
        error("auth-flows.init: totp_disable must be a function(user_id)")
    end
    _state.totp_disable = opts.totp_disable
    if opts.email_change_reauth ~= nil and type(opts.email_change_reauth) ~= "function" then
        error("auth-flows.init: email_change_reauth must be a function(req, user)")
    end
    _state.email_change_reauth = opts.email_change_reauth
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
            local s = _text.trim(stmt)
            if #s > 0 then db.exec(s) end
        end
    end)
    -- A table made before an email change was kept after its confirm (for
    -- revoke): add the columns that keep it. A failed ALTER (another instance
    -- added the column first) is re-checked, as session.lua does. A closure,
    -- not pcall(db.exec, ...): the _hull_* guard reads db.exec's caller.
    local cols = {}
    for _, n in ipairs(db.table_columns("_hull_auth_pending_email_changes") or {}) do
        cols[n] = true
    end
    for _, c in ipairs({ { "old_email", "old_email TEXT" },
                         { "confirmed_at", "confirmed_at INTEGER" } }) do
        if not cols[c[1]] then
            local ok, err = pcall(function()
                db.exec("ALTER TABLE _hull_auth_pending_email_changes ADD COLUMN " .. c[2])
            end)
            if not ok then
                local found = false
                for _, n in ipairs(db.table_columns("_hull_auth_pending_email_changes") or {}) do
                    if n == c[1] then found = true end
                end
                if not found then error(err, 0) end
            end
        end
    end

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

--- Standalone helpers - useful when an app needs to trigger one of
--- the flows from outside the standard routes (e.g. an admin
--- forcing a password reset for a user).

--- Programmatically trigger a verify-email send. Useful for admin
--- panels resending the link, or wiring this into a "re-send"
--- button on the app's login page. `verify_url_prefix` is the
--- full origin (`"https://app.example.com"`) - the module can't
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
                               _state.reset_ttl, reset_token_extra(user))
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
        -- create->get race guard (see handle_magic_link): a nil user would
        -- issue a sub=nil token and error in send_email.
        if not user then return end
    end
    local user_id = user_uid(user)
    local token = issue_token(user_id, ACTIONS.magic_link,
                               _state.magic_link_ttl,
                               { eb = email_binding(user) })
    local link = (magic_url_prefix or "")
                 .. _state.prefix .. "/magic-link/consume?token=" .. token
    send_email(email, "magic_link", {
        user = user, link = link, token = token,
    })
end

-- ── Test helpers (not public; exposed for unit tests) ──────────────

M._test = {
    origin_for         = origin_for,
    state              = _state,
    issue_token        = issue_token,
    consume_token      = consume_token,
    parse_token        = parse_token,
    mark_token_used    = mark_token_used,
    token_already_used = token_already_used,
    render_template    = render_template,
    gc_expired         = gc_expired,
    is_email_ish       = is_email_ish,
    parse_body         = parse_body,
    same_origin_request = same_origin_request,
    ACTIONS            = ACTIONS,
    email_rate_allow   = function(to) return email_rate_allow(to) end,
    email_rate_reset   = function()
        _email_rl = {}
        _email_rl_count = 0
    end,
    reset = function()
        _state.state_secret = nil
        _state.email_send       = nil
        _state.public_origin    = nil
        _state.trusted_hosts    = nil
        _state.trust_request_host = false
        _state.user_sanitize    = nil
        _state.warned_host_mismatch = false
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
        _state.verify_form_redirect    = nil
        _state.totp_disable            = nil
        _state.email_change_reauth     = nil
        _state.check_pwned_passwords   = false
        _state.pwned_endpoint          = nil
        _state.max_failed_logins       = 5
        _state.max_failed_logins_per_account = 50
        _state.lockout_duration        = 15 * 60
        _state.sign_in_log             = false
        _state.audit_log               = nil
        _state.on_password_reset       = nil
        _state.login_ratelimit         = false
        _state._initialized            = false
    end,
}

return M
