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
--     observables — never in production.
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

local crypto    = require("hull.crypto")
local envelope  = require("hull.crypto.envelope")
local db        = require("hull.db").default()
local time      = require("hull.time")
local json      = require("hull.json")
local pwned     = require("hull.web.pwned")
local audit_log = require("hull.web.middleware.audit-log")
local ratelimit = require("hull.web.middleware.ratelimit")

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
    -- Round-9 HIGH-1: URL origin gate. EVERY click-through URL (verify,
    -- magic-link, password-reset, email-change) is interpolated into a
    -- link inside an outbound email. Pre-round-9, the origin came
    -- straight from req.headers["x-forwarded-host"] / req.headers.host
    -- with no validation — an attacker submitting password-reset/request
    -- for victim's email with `Host: attacker.com` got the victim a
    -- "reset your password" mail pointing at attacker.com/auth/...?token=
    -- whose click leaked a valid action-bound reset token. Trivial
    -- account takeover.
    --
    -- Init now REQUIRES one of:
    --   * `public_origin = "https://app.example.com"` — single canonical
    --     URL; used as the sole authority for built links. The most
    --     common shape; the right answer for ~every non-multi-tenant app.
    --   * `trusted_hosts = {"app.example.com", "alt.example.com"}` —
    --     allowlist. req.headers.host must match (exactly) or the URL
    --     build refuses. Multi-tenant deployments where each customer
    --     has a different domain.
    --   * `trust_request_host = true` — DEV/TEST escape hatch. Falls
    --     back to the pre-round-9 behaviour: uses req.headers.host
    --     directly. Logs a one-shot warning at init. NEVER pass this
    --     in production — that's exactly the foot-gun this gate exists
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

    -- Flow-completion audit events (opt-in). When sign_in_log = true,
    -- auth-flows records password_reset_completed / email_change_revoked
    -- / email_changed into _hull_audit_log via hull/web/middleware/
    -- audit-log. Pair with on_password_reset to revoke existing
    -- sessions on reset (apps typically wire it as
    -- `function(req,res,user) session.destroy_all(user.id) end`).
    --
    -- LOGIN events and new-device detection are NOT emitted here
    -- anymore — they move to hull/web/middleware/session's
    -- login_handler factory (audit_log + on_new_device opts), so a
    -- single seam covers both password and OAuth logins.
    sign_in_log         = false,
    audit_log           = nil,   -- module handle, lazy-required when
                                 -- sign_in_log is enabled
    on_password_reset   = nil,

    -- Login rate limit (opt-in). When login_ratelimit is truthy, an
    -- hull/web/middleware/ratelimit middleware is installed on
    -- POST /auth/login BEFORE the handler. Defaults to 20 requests
    -- per IP per 5 minutes — tuned to make brute-force impractical
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
    -- orthogonal — a botnet defeats per-IP but not per-recipient.
    --
    -- Shape: { limit = N, window = SECONDS }. Pass `false` to
    -- disable. In-memory sliding window; resets on restart.
    -- Bounded to email_rate_limit_max_entries unique recipients.
    email_rate_limit    = { limit = 3, window = 900 },
    email_rate_limit_max_entries = 10000,

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
    local rc = db.insert_if_absent(
        "_hull_auth_used_tokens",
        { "token_hash" },
        { "token_hash", "used_at", "expires_at" },
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

-- Per-recipient email send rate limit. Sliding window keyed by
-- lower-cased recipient. Blocked sends are dropped silently so
-- the response shape stays enumeration-safe.
local _email_rl = {}
local _email_rl_count = 0

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
        if _email_rl_count > (_state.email_rate_limit_max_entries or 10000) then
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
-- was the wrong shape — apps with `totp_secret`, `recovery_codes`,
-- `api_key`, `oauth_refresh_token` etc. on their user records
-- leaked them into on_login + email template ctx.user). Default
-- allowlist is the canonical auth surface: id, email,
-- email_verified. Apps that need to expose more (display_name,
-- avatar_url, roles, etc.) pass `user_sanitize = function(user)
-- return {...} end` which gets the raw user and returns what's
-- safe to surface. Breaking change for apps reading custom user
-- fields in templates / on_login — they must wire user_sanitize.
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

-- Round-9 HIGH-1: build a click-through URL origin from validated
-- sources only. public_origin (when set) wins unconditionally;
-- otherwise the request's host header must match a trusted_hosts
-- entry exactly. If neither check admits a value, raise — letting
-- a request through with a header-derived origin reintroduces the
-- host-injection class. init.lua already validates that one of
-- public_origin / trusted_hosts is set, so the raise is only
-- reachable when an attacker submits a request whose host header
-- isn't in the allowlist.
local function origin_for(req)
    if _state.public_origin then
        return _state.public_origin
    end
    local h = (req and req.headers) or {}
    local host = h["x-forwarded-host"] or h.host
    -- Round-10 MEDIUM-6: normalize before allowlist comparison.
    -- X-Forwarded-Host can be a chain ("a.com, internal-lb") on
    -- nginx+ALB deployments — take the leftmost (client-facing)
    -- entry. Then strip an optional :PORT suffix so apps deployed
    -- on non-standard ports without a proxy that strips it match
    -- their bare-hostname allowlist entry.
    --
    -- Round-11 HIGH-3: IPv6 literals (`[::1]:8080`) are bracketed
    -- per RFC 3986. The naive first-colon split eats the IPv6
    -- colons; bare host becomes `[`. Detect the bracket and pull
    -- the literal whole (keep brackets so it matches a bracketed
    -- allowlist entry), then strip any trailing `:PORT`.
    if type(host) == "string" then
        local comma = host:find(",", 1, true)
        if comma then host = host:sub(1, comma - 1) end
        host = host:match("^%s*(.-)%s*$")
        if host:sub(1, 1) == "[" then
            local close = host:find("]", 1, true)
            if close then
                host = host:sub(1, close)
            elseif not _state.warned_malformed_ipv6 then
                -- Round-12 LOW-5: surface the bad Host header on its
                -- own one-shot warn so it doesn't consume the
                -- generic warned_host_mismatch slot. The mismatch
                -- itself still fires below; this just adds a
                -- diagnostic line pointing at the malformation.
                _state.warned_malformed_ipv6 = true
                local log = require("hull.log")
                log.warn("auth-flows: malformed IPv6 literal in "
                      .. "Host header (no closing ']'): '"
                      .. tostring(host) .. "'. RFC 3986 requires "
                      .. "IPv6 literals to be bracketed.")
            end
        else
            local colon = host:find(":", 1, true)
            if colon then host = host:sub(1, colon - 1) end
        end
    end
    if type(host) == "string" and host ~= ""
       and _state.trusted_hosts then
        for _, allowed in ipairs(_state.trusted_hosts) do
            if host == allowed then
                -- Use the raw (un-normalized) host:port for the URL
                -- so non-standard-port apps generate working links.
                local raw_host = h["x-forwarded-host"] or h.host
                local raw_comma = raw_host:find(",", 1, true)
                if raw_comma then
                    raw_host = raw_host:sub(1, raw_comma - 1)
                end
                raw_host = raw_host:match("^%s*(.-)%s*$")
                local proto = _state.trust_proxy
                    and (h["x-forwarded-proto"] or "https") or "https"
                return proto .. "://" .. raw_host
            end
        end
    end
    -- trust_request_host opt-out (dev/test). Last resort; the init
    -- warning fires once so operators can spot it in startup logs.
    if _state.trust_request_host and type(host) == "string"
       and host ~= "" then
        local raw_host = h["x-forwarded-host"] or h.host
        local raw_comma = raw_host:find(",", 1, true)
        if raw_comma then
            raw_host = raw_host:sub(1, raw_comma - 1)
        end
        raw_host = raw_host:match("^%s*(.-)%s*$")
        local proto = _state.trust_proxy
            and (h["x-forwarded-proto"] or "http") or "http"
        return proto .. "://" .. raw_host
    end
    -- We get here only if a request lands with a host not in the
    -- allowlist AND trust_request_host is off. Refuse to build a
    -- URL; the handler returns the same generic enumeration-safe
    -- response without actually sending the link.
    --
    -- Round-11 LOW-10: one-shot warn the first time this fires.
    -- Pre-fix, a misconfigured trusted_hosts produced 100% silent
    -- no-mail — operators learned from user complaints. The
    -- enumeration-safe contract means we can't 4xx the request,
    -- but a single log line at startup is enough for ops to spot
    -- the misconfig.
    if not _state.warned_host_mismatch then
        _state.warned_host_mismatch = true
        local raw = h["x-forwarded-host"] or h.host or "(nil)"
        local hosts = _state.trusted_hosts
        local list = "(none configured)"
        if hosts then
            list = table.concat(hosts, ", ")
        end
        local log = require("hull.log")
        log.warn("auth-flows: origin_for refused host '"
              .. tostring(raw) .. "' (normalized: '"
              .. tostring(host or "") .. "'). trusted_hosts = ["
              .. list .. "]. URL build skipped; subsequent email "
              .. "sends to this user-flow will be silently dropped "
              .. "until the host is added. Set public_origin or "
              .. "trust_request_host = true to override.")
    end
    return nil
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
    --
    -- Round-9 HIGH-2: when the previous lockout window has expired
    -- (locked_until IS NOT NULL AND < now) but the row hasn't been
    -- gc'd yet (gc_expired needs last_failed_at < now-86400), the
    -- stale failed_count = max-from-prior-cycle would push the
    -- expression `failed_count + 1 >= max` straight into a fresh
    -- lockout on the user's NEXT bad attempt. Net pre-fix: one
    -- attempt per 15-min window for the next 24h, not five. Reset to
    -- 1 when the prior window has elapsed.
    db.exec(
        "INSERT INTO _hull_auth_login_attempts "
        .. "(user_id, failed_count, last_failed_at, locked_until) "
        .. "VALUES (?, 1, ?, NULL) "
        .. "ON CONFLICT(user_id) DO UPDATE SET "
        .. "  failed_count = CASE "
        .. "    WHEN locked_until IS NOT NULL AND locked_until < ? "
        .. "      THEN 1 "
        .. "    ELSE failed_count + 1 "
        .. "  END, "
        .. "  last_failed_at = ?, "
        .. "  locked_until = CASE "
        .. "    WHEN locked_until IS NOT NULL AND locked_until < ? "
        .. "      THEN NULL "
        .. "    WHEN failed_count + 1 >= ? "
        .. "      THEN ? + ? "
        .. "    ELSE NULL "
        .. "  END",
        { user_id, now,
          now,           -- failed_count CASE: window-expired check
          now,           -- last_failed_at
          now,           -- locked_until CASE: window-expired check
          _state.max_failed_logins, now, _state.lockout_duration })
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
-- session.login_handler — see hull/web/middleware/session.lua's
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
    -- 256 char upper bound prevents PBKDF2 amplification DoS — a
    -- 10 MB submitted password would hash for multiple seconds at
    -- the default 600k iters. 256 covers any realistic passphrase
    -- (bcrypt's hard limit is 72 for comparison).
    if type(body.password) ~= "string"
       or #body.password < 8 or #body.password > 256 then
        return res:status(400):json({ error = "invalid password length" })
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
    local origin = origin_for(req)
    if origin then
        local verify_url = origin .. _state.prefix
                           .. "/verify?token=" .. token
        send_email(body.email, "welcome", {
            user = user, verify_url = verify_url, token = token,
        })
    end
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
    local origin = origin_for(req)
    if origin then
        local verify_url = origin .. _state.prefix
                           .. "/verify?token=" .. token
        send_email(body.email, "welcome", {
            user = user, verify_url = verify_url, token = token,
        })
    end
    res:json({ ok = true })
end

local function handle_verify(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.verify_email)
    if not env then
        return secure_html(res):status(400):html("verification failed: " .. (err or "?"))
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
    secure_html(res):html(default_totp_form_html(token))
end

local function handle_login(req, res)
    local body = parse_body(req)
    -- 256 char upper bound matches register; prevents PBKDF2
    -- amplification DoS via mega-passwords. Generic error keeps
    -- enumeration-safety (over-length is just another wrong cred).
    if not is_email_ish(body.email)
       or type(body.password) ~= "string"
       or #body.password > 256 then
        return res:status(400):json({ error = "invalid credentials" })
    end
    local user = _state.user_find_by_email(body.email)
    -- Lockout: when the user exists AND is currently locked, short-
    -- circuit to the SAME 401 + "invalid credentials" that the wrong-
    -- password branch returns. Round-8 HIGH-4: prior code returned
    -- 429 + Retry-After in this branch, which leaked account
    -- existence — an attacker could deliberately trip a lockout
    -- against a candidate address (5 bad guesses) and then enumerate
    -- registered emails by observing 429 vs 401. The locked state is
    -- preserved internally (the counter still ticks, the user still
    -- can't log in until the window expires) but the wire response is
    -- now indistinguishable from a wrong-password reply. Apps that
    -- want to surface "you're locked, try again in N seconds" UX to a
    -- user who's already authenticated through a different channel
    -- (e.g. mobile app) can read the counter via the (private)
    -- lockout_remaining helper on their own.
    local pre_locked = user and lockout_remaining(user_uid(user)) > 0
    -- Run verify_password unconditionally when enumeration_safe is on,
    -- using a pre-computed dummy hash on the unknown-email branch so
    -- network timing is identical between known and unknown emails.
    -- See init() for the dummy hash + the threat model. Opt-out via
    -- enumeration_safe = false (test fixtures only).
    local pwhash = (user and user.password_hash) or _state._dummy_pwhash
    local pw_ok  = (_state.enumeration_safe or user ~= nil)
                   and crypto.verify_password(body.password, pwhash)
    if pre_locked or not user or not user.password_hash or not pw_ok then
        if user and not pre_locked then bump_failed_login(user_uid(user)) end
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
    finish_login(req, res, user, "password")
end

local function handle_logout(req, res)
    -- We don't know the user_id here without inspecting the
    -- session — that's the app's responsibility. Apps that want
    -- a "logout" event in the audit log can call audit_log.record
    -- inside their on_logout callback.
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
    local origin = origin_for(req)
    if origin then
        local link = origin .. _state.prefix
                     .. "/magic-link/consume?token=" .. token
        send_email(body.email, "magic_link", {
            user = user, link = link, token = token,
        })
    end
    res:json({ ok = true })
end

local function handle_magic_link_consume(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.magic_link)
    if not env then
        return secure_html(res):status(400):html("magic link failed: " .. (err or "?"))
    end
    local user = _state.user_get(env.sub)
    if not user then
        return secure_html(res):status(400):html("magic link failed")
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
    finish_login(req, res, user, "magic_link")
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
    -- 2FA path — record both factors. Apps reading audit logs
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
    local token = issue_token(user_uid(user),
        ACTIONS.password_reset, _state.reset_ttl)
    local origin = origin_for(req)
    if origin then
        local link = origin .. _state.prefix
                     .. "/password-reset/confirm?token=" .. token
        send_email(body.email, "password_reset", {
            user = user, link = link, token = token,
        })
    end
    res:json({ ok = true })
end

local function handle_password_reset_confirm(req, res)
    local body = parse_body(req)
    -- Same upper bound as handle_register; see comment there.
    if type(body.password) ~= "string"
       or #body.password < 8 or #body.password > 256 then
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
    if not user then
        return res:status(400):json({ error = "reset failed" })
    end
    _state.user_set_password(env.sub, crypto.hash_password(body.password))
    -- A successful reset also unlocks the account: the user
    -- demonstrably controls the email, so any prior lockout is
    -- moot. (If they don't reset, the lockout window expires
    -- naturally per lockout_duration.)
    clear_failed_logins(env.sub)
    -- Audit + give the app a chance to invalidate existing
    -- sessions. The recommended on_password_reset implementation
    -- is `function(req,res,user) session.destroy_all(user.id) end`;
    -- apps that want to keep the current session can filter it
    -- out via session.destroy_others instead.
    emit_event(env.sub, "password_reset_completed", req)
    if _state.on_password_reset then
        pcall(_state.on_password_reset, req, res, user)
    end
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
    -- destroyed the prior pending row — when the user (or attacker)
    -- clicked a stale link, email_change_confirm rejected at the
    -- `new_email` mismatch guard, killing the legitimate flow with
    -- a generic error and no retry path. Force the user to either
    -- click revoke or wait email_change_ttl. Expired pending rows
    -- are reaped by gc_expired so the user isn't blocked forever
    -- if they abandoned the prior attempt.
    local existing = db.query(
        "SELECT new_email FROM _hull_auth_pending_email_changes "
        .. "WHERE user_id = ? AND expires_at > ? LIMIT 1",
        { user_id, time.now() })
    if existing and #existing > 0 then
        return res:status(409):json({
            error = "pending email change exists",
            new_email = existing[1].new_email,
        })
    end

    local now = time.now()
    local token = issue_token(user_id, ACTIONS.email_change,
        _state.email_change_ttl, { new_email = body.new_email })
    local token_hash = crypto.sha256(token)
    db.upsert(
        "_hull_auth_pending_email_changes",
        { "user_id" },
        { "user_id", "new_email", "token_hash", "created_at", "expires_at" },
        { user_id, body.new_email, token_hash, now,
          now + _state.email_change_ttl })

    local user = _state.user_get(user_id)
    local link = origin .. _state.prefix
                 .. "/email-change/confirm?token=" .. token
    -- Send to the NEW address — proves the user controls it.
    send_email(body.new_email, "email_change", {
        user = user, link = link, token = token,
        new_email = body.new_email,
    })
    -- Defense in depth: notify the OLD address with a revoke link
    -- so a stolen session cookie can't quietly move the account.
    -- Opt-in by providing templates.email_change_notify; apps
    -- without the template keep the v1 behavior.
    if _state.templates.email_change_notify then
        local revoke_tok = issue_token(user_id,
            ACTIONS.email_change_revoke, _state.email_change_ttl)
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

-- GET /auth/email-change/revoke?token=... — consumed by the
-- OLD-address holder to cancel a pending email change. Single-use
-- via the same _hull_auth_used_tokens table; deletes the pending
-- row and (for paranoia) burns any matching email_change token
-- whose envelope is still in flight by deleting BOTH rows.
local function handle_email_change_revoke(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.email_change_revoke)
    if not env then
        return secure_html(res):status(400):html("revoke failed: " .. (err or "?"))
    end
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            { env.sub })
    emit_event(env.sub, "email_change_revoked", req,
               { metadata = { by = "old_address" } })
    gc_expired()
    secure_html(res):html("Email change canceled.")
end

local function handle_email_change_confirm(req, res)
    local token = req.query and req.query.token
    local env, err = consume_token(token, ACTIONS.email_change)
    if not env then
        return secure_html(res):status(400):html("email change failed: " .. (err or "?"))
    end
    local user = _state.user_get(env.sub)
    if not user then
        return secure_html(res):status(400):html("email change failed")
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
        return secure_html(res):status(400):html("email change failed")
    end
    local old_email = user.email
    _state.user_set_email(env.sub, env.new_email)
    _state.user_set_email_verified(env.sub, true)
    db.exec("DELETE FROM _hull_auth_pending_email_changes WHERE user_id = ?",
            { env.sub })
    emit_event(env.sub, "email_changed", req,
               { metadata = { old_email = old_email,
                              new_email = env.new_email } })
    gc_expired()
    res:redirect(_state.verify_redirect)
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
            -- Per-IP key. X-Forwarded-For can be a chain
            -- ("client, proxy1, proxy2") when behind multiple
            -- reverse proxies; the leftmost entry is the client
            -- IP the edge proxy observed. Using the whole chain
            -- as the bucket key would let a single client with
            -- rotating downstream proxies mint a new bucket per
            -- request. Take the first comma-separated value and
            -- trim whitespace. App-supplied opts.key still wins.
            key    = rl_opts.key or function(req)
                local xff = req.headers and req.headers["x-forwarded-for"]
                if _state.trust_proxy and xff and xff ~= "" then
                    local comma = xff:find(",", 1, true)
                    local first = comma and xff:sub(1, comma - 1) or xff
                    local trimmed = first:match("^%s*(.-)%s*$")
                    if trimmed and trimmed ~= "" then return trimmed end
                end
                return req.remote_addr or "_anon"
            end,
        })
        app.use("POST", p .. "/register", mw)
        app.use("POST", p .. "/login", mw)
        app.use("POST", p .. "/magic-link", mw)
        app.use("POST", p .. "/password-reset/request", mw)
    end

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
--- Build a turnkey adapter for the 6 `user_*` callbacks against a
-- "standard" users-table schema. Apps with a vanilla schema can
-- pass the result as `opts.users` to M.init and skip ~30 lines of
-- thin DB wrappers. Apps with a custom schema either override
-- single callbacks (opts.user_create wins over opts.users.create)
-- or skip the adapter entirely.
--
-- The adapter is **DB-backend-agnostic** — it issues standard
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
--   * `table`   — table name (default `"users"`).
--   * `id_gen`  — `function() -> string` for new ids (default:
--                 32 hex chars from crypto.random(16)).
-- @treturn table  Six fields: find_by_email, get, create,
--                 set_password, set_email, set_email_verified.
function M.standard_users(opts)
    opts = opts or {}
    local tbl    = opts.table or "users"
    local id_gen = opts.id_gen or function()
        return crypto.hex_encode(crypto.random(16))
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
    if type(opts.state_secret) ~= "string" or #opts.state_secret < 32 then
        error("auth-flows.init: state_secret must be a string >= 32 bytes")
    end
    if type(opts.email_send) ~= "function" then
        error("auth-flows.init: email_send(to, subject, html, text) required")
    end
    if type(opts.templates) ~= "table" then
        error("auth-flows.init: templates table required")
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
        log.warn("auth-flows: trust_request_host = true — falling back to "
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
        -- Round-11 MEDIUM-4: trusted_hosts entries must be bare hosts
        -- (no :PORT). origin_for strips :PORT from the incoming Host
        -- before comparison, so an entry like "app.com:8080" never
        -- matches and the deployment silently sends zero emails. Catch
        -- the defensive-typo at init. IPv6 bracketed literals
        -- ([::1]) are allowed — colons inside the brackets are part
        -- of the host.
        for _, h in ipairs(opts.trusted_hosts) do
            if type(h) ~= "string" or h == "" then
                error("auth-flows.init: trusted_hosts entries must be "
                      .. "non-empty strings (got " .. type(h) .. ")")
            end
            if h:sub(1, 1) ~= "[" and h:find(":", 1, true) then
                error("auth-flows.init: trusted_hosts entry '" .. h
                      .. "' contains ':' — entries must be bare host "
                      .. "names; the URL preserves the request's port "
                      .. "automatically. IPv6 literals must be "
                      .. "bracketed (e.g. \"[::1]\").")
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
    -- bulk adapter — its 6 functions become the defaults;
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

    -- crypto.hmac_sha256 takes the key as a hex string; we encode
    -- once at init and reuse the hex form per request.
    --
    -- bytes_to_hex_local mirrors stdlib/js/hull/web/auth-flows.js
    -- `bytesToHex`. Hand-rolled because crypto.hex_encode treats its
    -- input as a *string* that's passed via Lua's C boundary; on the
    -- JS side the equivalent UTF-8-inflates code points >= 0x80,
    -- producing a different hex digest. State secrets that contain
    -- any byte >= 0x80 (e.g. a passphrase with non-ASCII) would
    -- silently derive different HMAC keys per runtime, breaking the
    -- byte-identical wire-format guarantee the JS header promises
    -- ("a Lua-Hull → JS-Hull migration works without re-issuing
    -- pending tokens"). string.byte iterates the raw bytes.
    local function bytes_to_hex_local(s)
        local n = #s
        local out = {}
        for i = 1, n do
            out[i] = string.format("%02x", string.byte(s, i))
        end
        return table.concat(out)
    end
    _state.state_secret_hex = bytes_to_hex_local(opts.state_secret)
    _state.email_send       = opts.email_send
    _state.public_origin    = opts.public_origin
    _state.trusted_hosts    = opts.trusted_hosts
    _state.trust_request_host = opts.trust_request_host == true
    _state.trust_proxy = opts.trust_proxy == true
    -- Round-12 MEDIUM-1: reset the one-shot host-mismatch warn so a
    -- hot-reload that fixes / changes the allowlist gets a fresh
    -- diagnostic on the next bad host. Without this, the warn fires
    -- only once per process — an operator who "fixes" the config
    -- but introduces a new typo wouldn't see the second warn.
    _state.warned_host_mismatch = false
    _state.warned_malformed_ipv6 = false
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
    email_rate_allow   = function(to) return email_rate_allow(to) end,
    email_rate_reset   = function()
        _email_rl = {}
        _email_rl_count = 0
    end,
    reset = function()
        _state.state_secret_hex = nil
        _state.email_send       = nil
        _state.public_origin    = nil
        _state.trusted_hosts    = nil
        _state.trust_request_host = false
        _state.user_sanitize    = nil
        _state.warned_host_mismatch = false
        _state.warned_malformed_ipv6 = false
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
        _state.sign_in_log             = false
        _state.audit_log               = nil
        _state.on_password_reset       = nil
        _state.login_ratelimit         = false
        _state._initialized            = false
    end,
}

return M
