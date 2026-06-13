-- Sign-in events e2e fixture (Lua). auth-flows + audit-log +
-- session: enables sign_in_log, wires onNewDevice (records a
-- captured email), and onPasswordReset (revokes all sessions).
-- The e2e drives login from two "browsers" (different UAs) to
-- exercise the new-device path + per-device list.

app.manifest({
    name = "sign-in-events-fixture",
    modules = {
        "hull/web/auth-flows@1",
        "hull/web/middleware/audit-log@1",
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
local audit_log = require("hull.web.middleware.audit-log")
local session   = require("hull.web.middleware.session")
local cookie    = require("hull.web.cookie")

session.init()
audit_log.init({ retain_days = 365 })

local users_by_email = {}
local users_by_id    = {}
local next_id        = 0
local sent_emails    = {}
local new_device_alerts = {}

local function user_create(email, pwhash)
    next_id = next_id + 1
    local id = "u" .. tostring(next_id)
    local u = { id = id, email = email, password_hash = pwhash,
                email_verified = false }
    users_by_email[email] = u
    users_by_id[id]      = u
    return id
end

authflows.init({
    state_secret = "fixture-state-secret-aaaaaaaaaaaa",
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
        local sid = session.create(
            { user_id = user.id, email = user.email },
            { req = req })
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
    -- Sign-in events on. Wire the callbacks.
    sign_in_log = true,
    on_new_device = function(_req, _res, user)
        -- Don't call os.time() — `os` is sandboxed out. Timestamp
        -- isn't needed for the test anyway.
        new_device_alerts[#new_device_alerts + 1] = {
            user_id = user.id, email = user.email,
        }
    end,
    on_password_reset = function(_req, _res, user)
        -- Recommended pattern: revoke all sessions on reset.
        session.destroy_all(user.id)
    end,
})

authflows.routes(app)

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

-- ── Debug endpoints ────────────────────────────────────────────────

app.get("/_emails",         function(_r, res) res:json(sent_emails) end)
app.post("/_emails/clear",  function(_r, res) sent_emails = {}; res:json({ ok = true }) end)
app.get("/_new_devices",    function(_r, res) res:json(new_device_alerts) end)
app.post("/_new_devices/clear", function(_r, res)
    new_device_alerts = {}; res:json({ ok = true })
end)

app.get("/_me", function(req, res)
    if not (req.ctx and req.ctx.session) then
        return res:status(401):json({ error = "not signed in" })
    end
    res:json(req.ctx.session)
end)

-- Per-user device + event lists. App owns the URL shape; this is
-- the typical wiring an app would expose under /account/devices.
app.get("/_devices", function(req, res)
    if not req.ctx or not req.ctx.user_id then
        return res:status(401):json({})
    end
    res:json({
        sessions = session.list_for_user(req.ctx.user_id),
        events   = audit_log.list(req.ctx.user_id, { limit = 50 }),
        devices  = audit_log.list_devices(req.ctx.user_id),
    })
end)

app.post("/_revoke_others", function(req, res)
    if not req.ctx or not req.ctx.user_id then
        return res:status(401):json({})
    end
    local n = session.destroy_others(req.ctx.session_id, req.ctx.user_id)
    res:json({ ok = true, revoked = n })
end)
