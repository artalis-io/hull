-- Auth-flows example: a tiny credentials-managed web app
-- demonstrating registration, email verification, login, logout,
-- magic-link sign-in, password reset, and email change — all
-- bundled by hull/web/auth-flows@1, all wired against a plain
-- `users` table the app owns.
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
local crypto    = require("hull.crypto")
local db        = require("hull.db")
local env       = require("hull.env")
local log       = require("hull.log")
local time      = require("hull.time")

session.init()  -- creates hull_sessions table

-- ── Users table helpers ─────────────────────────────────────────────
--
-- Thin wrappers over `users` SQL so the auth-flows callbacks below
-- stay readable. A real app would put these on its own user model
-- module; the example keeps them here for one-file legibility.

local function gen_id()
    local hex = ""
    local bytes = crypto.random(16)
    for i = 1, #bytes do
        hex = hex .. string.format("%02x", string.byte(bytes, i))
    end
    return hex
end

local function user_row(r)
    if not r then return nil end
    return {
        id             = r.id,
        email          = r.email,
        password_hash  = r.password_hash,
        email_verified = r.email_verified == 1,
    }
end

local function find_by_email(email)
    local rows = db.query("SELECT * FROM users WHERE email = ?", { email })
    return rows and rows[1] and user_row(rows[1]) or nil
end

local function get(id)
    local rows = db.query("SELECT * FROM users WHERE id = ?", { id })
    return rows and rows[1] and user_row(rows[1]) or nil
end

local function create(email, pwhash)
    local id = gen_id()
    local now = time.now()
    db.exec(
        "INSERT INTO users (id, email, password_hash, email_verified, "
        .. "created_at, updated_at) VALUES (?, ?, ?, 0, ?, ?)",
        { id, email, pwhash, now, now })
    return id
end

local function set_password(id, pwhash)
    db.exec("UPDATE users SET password_hash = ?, updated_at = ? WHERE id = ?",
            { pwhash, time.now(), id })
end

local function set_email(id, email)
    db.exec("UPDATE users SET email = ?, updated_at = ? WHERE id = ?",
            { email, time.now(), id })
end

local function set_email_verified(id, verified)
    db.exec("UPDATE users SET email_verified = ?, updated_at = ? WHERE id = ?",
            { verified and 1 or 0, time.now(), id })
end

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

-- env.get throws "env not configured" at module load time because
-- env_cfg gets wired AFTER the manifest is extracted (which is
-- AFTER this top-level code has run). pcall lets the same file
-- run in both contexts: dev/test uses the fallback, prod sets
-- AUTH_FLOWS_SECRET in the environment and env.get works.
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
    user_find_by_email      = find_by_email,
    user_get                = get,
    user_create             = create,
    user_set_password       = set_password,
    user_set_email          = set_email,
    user_set_email_verified = set_email_verified,
    on_login = function(req, res, user)
        -- Issue an app session. The session module's create() puts
        -- the row in hull_sessions and returns a session ID we set
        -- in a cookie. Real apps would attach more user fields to
        -- the session (display name, role, etc.).
        local sid = session.create({ user_id = user.id, email = user.email })
        res:header("Set-Cookie", cookie.serialize("session", sid,
            { path = "/", httponly = true, samesite = "Lax" }))
        res:json({ ok = true, user_id = user.id, email = user.email })
    end,
    on_logout = function(req, res)
        local cookies = cookie.parse(req.headers.cookie or "")
        if cookies.session then session.destroy(cookies.session) end
        res:header("Set-Cookie", cookie.clear("session", { path = "/" }))
        res:json({ ok = true })
    end,
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
            req.ctx.session = data
            req.ctx.user_id = data.user_id
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
--       -- totp.verify returns (ok, kind) — the second return is
--       -- discarded by Lua's single-value assignment in auth-flows,
--       -- so a plain delegate works. From JS the tuple-as-array
--       -- needs explicit `[0]` unwrapping (see CLAUDE.md).
--       totp_verify = function(user, code)
--           return totp.verify(user.id, code)
--       end,
--   })
--
-- See tests/fixtures/auth_flows_2fa_lua/ for an end-to-end fixture
-- and tests/e2e_auth_flows_2fa.sh for the wire flow.
