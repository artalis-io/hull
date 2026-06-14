-- Auth-flows + TOTP 2FA composition fixture (Lua). Same shape as
-- tests/fixtures/auth_flows_lua but with `enable_totp = true` wired
-- and the totp module composed via the user_totp_enrolled +
-- totp_verify callbacks.
--
-- Debug endpoints used by tests/e2e_auth_flows_2fa.sh:
--
--   POST /_totp_enroll  {email}        -> {secret, recovery_codes}
--   POST /_totp_confirm {email, code}  -> {ok}
--   GET  /_emails                      -> captured email log
--   POST /_emails/clear                -> reset email log
--   GET  /_me                          -> current session payload

app.manifest({
    name = "auth-flows-2fa-fixture",
    modules = {
        "hull/web/auth-flows@1",
        "hull/web/middleware/totp@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/http-server@1",
        "hull/db@1",
        "hull/log@1",
        "hull/json@1",
        "hull/time@1",
    },
})

local authflows = require("hull.web.auth-flows")
local totp      = require("hull.web.middleware.totp")
local session   = require("hull.web.middleware.session")
local cookie    = require("hull.web.cookie")

session.init()
totp.init({ issuer = "auth-flows-2fa-test" })

local users_by_email = {}
local users_by_id    = {}
local next_id        = 0
local sent_emails    = {}

local function user_create(email, pwhash)
    next_id = next_id + 1
    local id = "u" .. tostring(next_id)
    local u = { id = id, email = email, password_hash = pwhash,
                email_verified = false }
    users_by_email[email] = u
    users_by_id[id] = u
    return id
end

authflows.init({
    state_secret = "fixture-state-secret-aaaaaaaaaaaa",
    email_rate_limit = false,  -- see auth_flows_lua/app.lua
    email_send = function(to, subject, html, text)
        sent_emails[#sent_emails + 1] = {
            to = to, subject = subject, text = text or html,
        }
    end,
    templates = {
        welcome = function(c) return {
            subject = "Welcome", text = "verify: " .. c.verify_url } end,
        verify = function(c) return {
            subject = "Verify",  text = "verify: " .. (c.verify_url or c.link or "?") } end,
        magic_link = function(c) return {
            subject = "Sign in", text = "link: " .. c.link } end,
        password_reset = function(c) return {
            subject = "Reset",   text = "link: " .. c.link } end,
        email_change = function(c) return {
            subject = "Confirm email change", text = "link: " .. c.link } end,
    },
    user_find_by_email      = function(email) return users_by_email[email] end,
    user_get                = function(id)    return users_by_id[id] end,
    user_create             = user_create,
    user_set_password       = function(id, pwhash)
        users_by_id[id].password_hash = pwhash
    end,
    user_set_email = function(id, email)
        local u = users_by_id[id]
        users_by_email[u.email] = nil
        u.email = email
        users_by_email[email] = u
    end,
    user_set_email_verified = function(id, v)
        users_by_id[id].email_verified = v
    end,
    -- TOTP composition: defer enrollment lookup + code verification
    -- to the totp module. Recovery codes flow through totp.verify
    -- transparently (canonicalized post-M2).
    enable_totp        = true,
    user_totp_enrolled = function(user_id) return totp.enrolled(user_id) end,
    totp_verify        = function(user, code)
        return totp.verify(user.id or user.user_id, code)
    end,
    on_login = function(req, res, user)
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

-- ── Debug endpoints ──────────────────────────────────────────────────

app.post("/_totp_enroll", function(req, res)
    local body = req.body and req.body or ""
    local json = require("hull.json")
    local b = json.decode(body) or {}
    local u = users_by_email[b.email]
    if not u then return res:status(404):json({ error = "no user" }) end
    local r = totp.enroll(u.id)
    res:json({
        secret = r.secret_base32,
        otpauth_url = r.otpauth_url,
        recovery_codes = r.recovery_codes,
    })
end)

app.post("/_totp_confirm", function(req, res)
    local body = req.body and req.body or ""
    local json = require("hull.json")
    local b = json.decode(body) or {}
    local u = users_by_email[b.email]
    if not u then return res:status(404):json({ error = "no user" }) end
    local ok, err = totp.confirm(u.id, b.code)
    if not ok then return res:status(400):json({ error = err or "no" }) end
    res:json({ ok = true })
end)

app.get("/_emails",         function(_r, res) res:json(sent_emails) end)
app.post("/_emails/clear",  function(_r, res) sent_emails = {}; res:json({ ok = true }) end)
app.get("/_me", function(req, res)
    if not (req.ctx and req.ctx.session) then
        return res:status(401):json({ error = "not signed in" })
    end
    res:json(req.ctx.session)
end)
