-- Auth-flows hardening fixture (Lua). Enables: account lockout
-- (with a 2-second window for fast tests), pwned-password check
-- pointing at a localhost HIBP mock the e2e spawns, and the
-- email-change notify+revoke template.
--
-- Debug endpoints (same convention as auth_flows_lua):
--   GET  /_emails           -> captured email log
--   POST /_emails/clear     -> reset email log
--   GET  /_me               -> current session payload

-- The e2e binds its HIBP mock to this fixed port so the fixture
-- can hardcode the endpoint. We can't pull a per-run port out of
-- the environment because Hull's env cap isn't wired until AFTER
-- top-level code runs (env.get at this point would throw).
local HIBP_MOCK_PORT = 39911

app.manifest({
    name = "auth-flows-hardening-fixture",
    modules = {
        "hull/web/auth-flows@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/http-server@1",
        "hull/db@1",
        "hull/log@1",
        "hull/json@1",
        "hull/time@1",
    },
    hosts = { "127.0.0.1" },
})

local authflows = require("hull.web.auth-flows")
local session   = require("hull.web.middleware.session")
local cookie    = require("hull.web.cookie")

session.init()

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

local hibp_mock = "http://127.0.0.1:" .. HIBP_MOCK_PORT .. "/range/"

authflows.init({
    state_secret = "fixture-state-secret-aaaaaaaaaaaa",
    email_rate_limit = false,  -- see auth_flows_lua/app.lua
    trust_request_host = true, -- see auth_flows_lua/app.lua (round-9 HIGH-1)
    email_send = function(to, subject, html, text)
        sent_emails[#sent_emails + 1] = {
            to = to, subject = subject, text = text or html,
        }
    end,
    templates = {
        welcome             = function(c) return { subject = "Welcome", text = "verify: " .. c.verify_url } end,
        verify              = function(c) return { subject = "Verify",  text = "verify: " .. (c.verify_url or c.link or "?") } end,
        magic_link          = function(c) return { subject = "Sign in", text = "link: " .. c.link } end,
        password_reset      = function(c) return { subject = "Reset",   text = "link: " .. c.link } end,
        email_change        = function(c) return { subject = "Confirm email change", text = "confirm: " .. c.link } end,
        email_change_notify = function(c) return {
            subject = "Email change requested",
            text = "Someone requested changing your email to "
                .. c.new_email .. ". If this wasn't you, click: " .. c.revoke_url,
        } end,
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
    -- Hardening config.
    max_failed_logins     = 3,    -- tighter for fast tests
    lockout_duration      = 2,    -- 2 seconds — survives the test
                                  -- bracket without slowing CI
    check_pwned_passwords = true,
    pwned_endpoint        = hibp_mock,
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

app.get("/_emails",         function(_r, res) res:json(sent_emails) end)
app.post("/_emails/clear",  function(_r, res) sent_emails = {}; res:json({ ok = true }) end)
app.get("/_me", function(req, res)
    if not (req.ctx and req.ctx.session) then
        return res:status(401):json({ error = "not signed in" })
    end
    res:json(req.ctx.session)
end)
