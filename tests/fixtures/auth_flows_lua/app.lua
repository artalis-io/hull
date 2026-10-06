-- Auth-flows e2e fixture (Lua). Same wiring as
-- examples/auth_flows/app.lua but with two debug endpoints the
-- e2e orchestrator drives:
--
--   GET /_emails           -> JSON array of {to, subject, text}
--                             entries captured by the in-memory
--                             email_send.
--   GET /_me               -> current session payload (or 401).
--
-- App user storage is a single in-process table (no migration
-- needed) so the e2e runs against `:memory:` SQLite and doesn't
-- leave state behind between runs.

app.manifest({
    name = "auth-flows-fixture",
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
})

local authflows = require("hull.web.auth-flows")
local session   = require("hull.web.middleware.session")
local cookie    = require("hull.web.cookie")
local crypto    = require("hull.crypto")
local time      = require("hull.time")

session.init()

-- In-memory user storage. Single-process, single-test-run scope.
local users_by_email = {}
local users_by_id    = {}
local next_id = 0
local sent_emails = {}
local slow_templates = false

local function user_create(email, pwhash)
    next_id = next_id + 1
    local id = "u" .. tostring(next_id)
    local u = {
        id = id, email = email, password_hash = pwhash,
        email_verified = 0,   -- a raw row: 0 / 1 (audit 6: Lua read 0 as true)
    }
    users_by_email[email] = u
    users_by_id[id] = u
    return id
end

authflows.init({
    state_secret = ("fixture-state-secret-aaaaaaaaaaaa"),  -- 32 chars
    -- Tests reuse the same `alice@example.test` across multiple flows
    -- (register, verify, magic-link, reset, email-change). Disable the
    -- per-recipient email rate limit so the fixture doesn't trip the
    -- default 3/15min gate. Round-8: a dedicated e2e_auth_flows_email_
    -- ratelimit.sh exercises the gate itself with a fresh process.
    email_rate_limit = false,
    -- Round-9 HIGH-1: e2e fixture binds an ephemeral port; we can't
    -- know the public_origin at init time. trust_request_host is the
    -- dev/test escape hatch; production apps MUST use public_origin
    -- or trusted_hosts. A dedicated e2e exercises the gate itself.
    trust_request_host = true,
    email_send = function(to, subject, html, text)
        sent_emails[#sent_emails + 1] = {
            to = to, subject = subject, text = text or html,
        }
    end,
    templates = {
        welcome = function(ctx)
            return { subject = "Welcome",
                     text = "verify: " .. ctx.verify_url }
        end,
        verify = function(ctx)
            return { subject = "Verify",
                     text = "verify: " .. (ctx.verify_url or ctx.link or "?") }
        end,
        magic_link = function(ctx)
            -- POST /_slow_templates: a 300 ms render, so the e2e can tell
            -- whether the mail is sent before or after the response.
            if slow_templates then
                local stop = time.now_ms() + 300
                while time.now_ms() < stop do end
            end
            return { subject = "Sign in", text = "link: " .. ctx.link }
        end,
        password_reset = function(ctx)
            return { subject = "Reset", text = "link: " .. ctx.link }
        end,
        email_change = function(ctx)
            return { subject = "Confirm email change",
                     text = "link: " .. ctx.link }
        end,
    },
    user_find_by_email = function(email) return users_by_email[email] end,
    -- user_get keeps the hash out of the model (a common adapter habit):
    -- auth-flows must read it through user_find_by_email (audit 6 M1).
    user_get           = function(id)
        local u = users_by_id[id]
        if not u then return nil end
        return { id = u.id, email = u.email, email_verified = u.email_verified }
    end,
    user_create        = user_create,
    user_set_password  = function(id, pwhash)
        users_by_id[id].password_hash = pwhash
    end,
    user_set_email = function(id, email)
        local u = users_by_id[id]
        users_by_email[u.email] = nil
        u.email = email
        users_by_email[email] = u
    end,
    user_set_email_verified = function(id, v)
        users_by_id[id].email_verified = v and 1 or 0
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

-- Session loader so /me + /auth/email-change can see req.ctx.user_id.
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

-- ── Debug endpoints (fixture-only) ──────────────────────────────────

-- require_verified_email = false without on_password_reset is refused at
-- init (audit 6 M2), before any state changes.
app.get("/_init_refuses_unverified_login", function(_req, res)
    local ok, err = pcall(authflows.init, {
        state_secret = ("fixture-state-secret-aaaaaaaaaaaa"),
        email_send = function() end,
        templates = {},
        require_verified_email = false,
    })
    res:json({ refused = not ok
        and tostring(err):find("on_password_reset", 1, true) ~= nil })
end)

app.get("/_emails", function(_req, res)
    res:json(sent_emails)
end)

app.post("/_emails/clear", function(_req, res)
    sent_emails = {}
    res:json({ ok = true })
end)

app.post("/_slow_templates", function(_req, res)
    slow_templates = true
    res:json({ ok = true })
end)

app.get("/_me", function(req, res)
    if not (req.ctx and req.ctx.session) then
        return res:status(401):json({ error = "not signed in" })
    end
    res:json(req.ctx.session)
end)
