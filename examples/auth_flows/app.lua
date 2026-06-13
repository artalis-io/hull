-- Auth-flows example: a tiny credentials-managed web app
-- demonstrating registration, email verification, login, logout,
-- magic-link sign-in, password reset, and email change — all
-- bundled by hull/web/auth-flows@1, all wired against a plain
-- `users` table the app owns.
--
-- The wiring below uses the turnkey helpers:
--   * `authflows.standard_users({...})` fills the 6 user_*
--     callbacks for the standard schema in one line. The adapter
--     is DB-backend-agnostic.
--   * `session.login_handler(cookie)` and `session.logout_handler(cookie)`
--     produce the on_login / on_logout pair, including session-
--     fixation defense (session.rotate on login).
--
-- Apps with a custom users schema or session shape can still
-- override individual callbacks (opts.user_create wins over
-- opts.users.create, etc.) — see the explicit-wiring fixture at
-- tests/fixtures/auth_flows_lua/app.lua for that style.
--
-- Run: AUTH_FLOWS_SECRET=$(head -c 32 /dev/urandom | base64) \
--      hull dev examples/auth_flows/app.lua -p 3000
--
-- Then visit http://localhost:3000/ for the demo nav. Outgoing
-- emails print to stderr — comment in the hull/email wiring at
-- the bottom of this file to actually deliver them.

app.manifest({
    name = "auth-flows-example",
    modules = {
        "hull/web/auth-flows@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/http-server@1",
        "hull/db@1",
        "hull/env@1",
        "hull/log@1",
        "hull/json@1",
        "hull/time@1",
    },
    env = { "AUTH_FLOWS_SECRET" },
})

local authflows = require("hull.web.auth-flows")
local session   = require("hull.web.middleware.session")
local cookie    = require("hull.web.cookie")
local env       = require("hull.env")
local log       = require("hull.log")

-- Init the sessions table BEFORE the login_handler factory below
-- inspects it (the factory errors clearly if session.init() hasn't
-- been called).
session.init()

-- ── Email templates ─────────────────────────────────────────────────
--
-- Each template is a function that receives a context table and
-- returns { subject, html?, text? }. Apps with a template engine
-- (hull/template, mustache, etc.) would render real HTML here; the
-- example uses plain text for clarity.

local templates = {
    welcome = function(ctx)
        return {
            subject = "Welcome to the demo — verify your email",
            text = "Hi " .. ctx.user.email .. ",\n\n"
                .. "Click to verify: " .. ctx.verify_url .. "\n\n"
                .. "Link expires in 24 hours.\n",
        }
    end,
    verify = function(ctx)
        return {
            subject = "Verify your email",
            text = "Click: " .. (ctx.verify_url or ctx.link or "?") .. "\n",
        }
    end,
    magic_link = function(ctx)
        return {
            subject = "Your sign-in link",
            text = "Click to sign in: " .. ctx.link .. "\n\n"
                .. "Link expires in 10 minutes.\n",
        }
    end,
    password_reset = function(ctx)
        return {
            subject = "Reset your password",
            text = "Click to reset: " .. ctx.link .. "\n\n"
                .. "Link expires in 1 hour. If you didn't request "
                .. "this, you can ignore it.\n",
        }
    end,
    email_change = function(ctx)
        return {
            subject = "Confirm your new email address",
            text = "Click to confirm changing your email to "
                .. ctx.new_email .. ": " .. ctx.link .. "\n",
        }
    end,
}

-- ── auth-flows wiring ────────────────────────────────────────────────

-- env.get throws at module load time when the manifest's env_cfg
-- hasn't been wired yet (top-level code runs before that). pcall
-- lets the same file run in both contexts: dev/test falls back to
-- the placeholder, prod sets AUTH_FLOWS_SECRET in the environment.
local function envget(k, default)
    local ok, v = pcall(env.get, k)
    if ok and v then return v end
    return default
end

authflows.init({
    state_secret = envget("AUTH_FLOWS_SECRET",
        "dev-only-secret-replace-me-aaaaa"),  -- 32 chars; refuse to use in prod
    -- Stdout email sender — replace with hull/email for real sending.
    -- See the commented block at the bottom of this file.
    email_send = function(to, subject, html, text)
        log.info("📧 to=" .. to .. " subject=" .. subject)
        log.info("--- email body ---\n" .. (text or html or "") .. "---")
    end,
    templates = templates,
    -- Turnkey user-table adapter for the standard schema. Custom
    -- schemas: skip `users = ...` and pass the 6 user_* callbacks
    -- directly. Mixed: pass `users` AND override one or two
    -- callbacks individually (explicit wins).
    users = authflows.standard_users({ table = "users" }),
    -- Turnkey login/logout handlers backed by session + cookie.
    -- session-fixation defense (session.rotate) is on by default.
    on_login  = session.login_handler(cookie),
    on_logout = session.logout_handler(cookie),
})

authflows.routes(app)

-- ── App routes ──────────────────────────────────────────────────────

-- A tiny session-loader middleware so /me can return the logged-in
-- user. Real apps would use hull/web/middleware/auth here.
app.use("*", "/*", function(req, _res)
    local cookies = cookie.parse(req.headers.cookie or "")
    if cookies.session then
        local data = session.load(cookies.session)
        if data then
            req.ctx = req.ctx or {}
            req.ctx.session    = data
            req.ctx.session_id = cookies.session
            req.ctx.user_id    = data.user_id
        end
    end
    return 0
end)

app.get("/", function(_req, res)
    res:html([[
<!doctype html><html><body style="font-family: sans-serif; max-width: 600px; margin: 2em auto;">
<h1>auth-flows demo</h1>
<p>POST endpoints under <code>/auth/*</code>. Click-through endpoints
   under <code>/auth/verify</code>, <code>/auth/magic-link/consume</code>,
   <code>/auth/email-change/confirm</code>. Emails print to the dev
   server's stderr.</p>
<ul>
  <li><code>POST /auth/register {email, password}</code></li>
  <li><code>POST /auth/login {email, password}</code></li>
  <li><code>POST /auth/magic-link {email}</code></li>
  <li><code>POST /auth/password-reset/request {email}</code></li>
  <li><code>POST /auth/password-reset/confirm {token, password}</code></li>
  <li><code>POST /auth/email-change {new_email}</code> (must be logged in)</li>
  <li><code>POST /auth/logout</code></li>
  <li><code>GET /me</code></li>
</ul>
</body></html>
    ]])
end)

app.get("/me", function(req, res)
    if not (req.ctx and req.ctx.session) then
        return res:status(401):json({ error = "not signed in" })
    end
    res:json(req.ctx.session)
end)

-- ── To send real email, swap the email_send callback above for: ─────
--
--   local email = require("hull.email")
--   email.init({ provider = "smtp", host = "smtp.example.com", ... })
--   ...
--   email_send = function(to, subject, html, text)
--       email.send({ to = to, subject = subject,
--                    html = html, text = text })
--   end,
--
-- See examples/email/ for the full provider setup.

-- ── Adding TOTP 2FA on top: ─────────────────────────────────────────
--
-- auth-flows composes with hull/web/middleware/totp via two
-- callbacks. With these set, a successful password login or
-- magic-link click for an enrolled user yields
--   { ok: true, pending_2fa: true, totp_token: "..." }
-- (JSON for POST /login, default HTML form for the magic-link GET)
-- and waits for POST /auth/totp-verify {token, code} before
-- handing off to on_login.
--
--   local totp = require("hull.web.middleware.totp")
--   totp.init({ issuer = "my-app" })
--   authflows.init({
--       ...
--       enable_totp = true,
--       user_totp_enrolled = function(uid) return totp.enrolled(uid) end,
--       totp_verify        = function(user, code)
--           return totp.verify(user.id, code)
--       end,
--   })
--
-- See tests/fixtures/auth_flows_2fa_lua/ for an end-to-end fixture
-- and tests/e2e_auth_flows_2fa.sh for the wire flow.

-- ── Adding Google OAuth on top: ─────────────────────────────────────
--
-- The OAuth on_login signature matches auth-flows after the
-- find_user split, so the SAME session.login_handler(cookie) wires
-- both auth sources.
--
--   local oauth = require("hull.web.middleware.oauth")
--   oauth.init({
--       state_secret = env.get("OAUTH_STATE_SECRET"),
--       providers = { google = {
--           preset        = "google",
--           client_id     = env.get("GOOGLE_CLIENT_ID"),
--           client_secret = env.get("GOOGLE_CLIENT_SECRET"),
--       }},
--       find_user = function(_provider, claims)
--           -- Look up by claims.sub / claims.email; create if new.
--           local u = users_by_email[claims.email]
--           if u then return u end
--           local id = users_adapter.create(claims.email, nil)
--           return users_adapter.get(id)
--       end,
--       on_login  = session.login_handler(cookie),
--       on_logout = session.logout_handler(cookie),
--   })
--   oauth.routes(app)
