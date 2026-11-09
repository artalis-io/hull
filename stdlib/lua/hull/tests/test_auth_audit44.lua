-- test_auth_audit44.lua - auth stdlib regressions from audit 44 (Lua).
--
-- oauth: state_ttl / jwks_ttl must be positive numbers; a failed JWKS fetch
-- is throttled like a fetched one (JWKS_MIN_REFRESH); a token reply that is
-- not JSON answers 502 instead of raising. session: the login audit-metadata
-- scrub reaches nested tables and matches camelCase / kebab-case keys; data
-- that cannot be decoded destroys the row instead of raising. auth-flows:
-- the request Host is matched to trusted_hosts without case; cleanup()
-- reaps the lockout rows unknown addresses leave. rbac: a malformed session
-- user id answers 401, and a role / permission argument is checked when the
-- middleware is made. auth: `optional` must be the literal true. The JS twin
-- is test_auth_audit44.js.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): auth-flows, session and rbac need the db.

local json     = require("hull.json")
local crypto   = require("hull.crypto")
local encoding = require("hull.encoding")
local cookie   = require("hull.web.cookie")
local oauth    = require("hull.web.middleware.oauth")
local session  = require("hull.web.middleware.session")
local rbac     = require("hull.web.middleware.rbac")
local auth     = require("hull.web.middleware.auth")
local af       = require("hull.web.auth-flows")

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

local function mkres()
    local r = { code = 200, headers = {} }
    function r.status(self, c) self.code = c; return self end
    function r.json(self, v) self.body = v; return self end
    function r.html(self, v) self.body = v; return self end
    function r.text(self, v) self.body = v; return self end
    function r.header(self, k, v) self.headers[k] = v; return self end
    function r.redirect(self, p, c) self.code = c or 302; self.location = p; return self end
    return r
end

local function mkreq(headers, ctx)
    return { method = "POST", path = "/x", headers = headers or { host = "app.test" },
             ctx = ctx or {}, remote_addr = "192.0.2.7", query = {}, params = {} }
end

-- ── oauth ──────────────────────────────────────────────────────────

local OT = __test_hooks("hull.web.middleware.oauth")
local HC = __test_hooks("hull._test_hooks").real("hull.http-client")

local function oauth_init(extra)
    OT.reset()
    local opts = { secret = ("s"):rep(32), base_url = "https://app.test",
                   providers = { g = { preset = "google", client_id = "cid" } } }
    for k, v in pairs(extra or {}) do opts[k] = v end
    oauth.init(opts)
end

-- The callback handler, as oauth.routes mounts it.
local function callback_handler()
    local got
    oauth.routes({ get = function(path, fn)
        if path:find("callback", 1, true) then got = fn end
    end, post = function() end })
    return got
end

local function callback_req()
    local v = OT.sign_state({ provider = "g", verifier = "v", state = "st", nonce = "n" })
    return { method = "GET", path = "/auth/g/callback",
             headers = { host = "app.test", cookie = "_oauth_state=" .. v },
             params = { provider = "g" }, query = { code = "c", state = "st" },
             ctx = {}, remote_addr = "192.0.2.7" }
end

local function b64u(s) return encoding.base64.encode(s, { url = true }) end
local ID_TOKEN = b64u('{"alg":"RS256","kid":"k1"}') .. "." .. b64u("{}") .. "." .. b64u("sig")

test("oauth_audit44.init_ttl_types", function()
    for _, k in ipairs({ "state_ttl", "jwks_ttl" }) do
        for _, bad in ipairs({ "600", -1, 0, 0 / 0, math.huge, {} }) do
            local ok, err = pcall(oauth_init, { [k] = bad })
            assert_eq(ok, false, k .. "=" .. tostring(bad) .. " refused")
            assert_eq(tostring(err):find(k, 1, true) ~= nil, true, tostring(err))
        end
        assert_eq(pcall(oauth_init, { [k] = 300 }), true, k .. " = 300 taken")
    end
    oauth_init({ state_ttl = 120 })
    local env = OT.verify_state(OT.sign_state({ provider = "g" }))
    assert_eq(type(env), "table", "a state signed under a numeric ttl verifies")
    OT.reset()
end)

test("oauth_audit44.jwks_failure_throttled", function()
    oauth_init()
    local gets = 0
    local real_get, real_post = HC.async.get, HC.async.post
    HC.async.get = function() gets = gets + 1; return { status = 503, body = "down" } end
    HC.async.post = function()
        return { status = 200, body = json.encode({ id_token = ID_TOKEN }) }
    end
    local h = callback_handler()
    local ok, err = pcall(function()
        for _ = 1, 3 do
            local res = mkres()
            h(callback_req(), res)
            assert_eq(res.code, 400, "auth failed")
        end
    end)
    HC.async.get, HC.async.post = real_get, real_post
    OT.reset()
    assert_eq(ok, true, tostring(err))
    assert_eq(gets, 1, "one JWKS fetch for three callbacks inside a minute")
end)

test("oauth_audit44.token_endpoint_non_json_is_502", function()
    oauth_init()
    local real_post = HC.async.post
    HC.async.post = function() return { status = 200, body = "<html>not json</html>" } end
    local h = callback_handler()
    local res = mkres()
    local ok, err = pcall(h, callback_req(), res)
    HC.async.post = real_post
    OT.reset()
    assert_eq(ok, true, "did not raise: " .. tostring(err))
    assert_eq(res.code, 502, "bad gateway")
end)

-- ── session ────────────────────────────────────────────────────────

test("session_audit44.scrub_nested_and_camel", function()
    session.init({ cleanup = false })
    local recorded
    local h = session.login_handler(cookie, {
        audit_log = { record = function(_, _, _, o) recorded = o.metadata end },
        audit_metadata = function()
            return {
                factors = "oauth:g",
                idToken = "it", accessToken = "at", ["refresh-token"] = "rt",
                PasswordHash = "ph",
                oauth = { provider = "g", tokens = { access_token = "at" },
                          claims = { sub = "x" }, nested = { idToken = "it", keep = 2 } },
                list = { { Secret = "s", keep = 1 } },
            }
        end,
    })
    h(mkreq(), mkres(), { id = 44 })
    assert_eq(type(recorded), "table", "recorded")
    assert_eq(recorded.factors, "oauth:g", "kept")
    assert_eq(recorded.idToken, nil, "idToken")
    assert_eq(recorded.accessToken, nil, "accessToken")
    assert_eq(recorded["refresh-token"], nil, "refresh-token")
    assert_eq(recorded.PasswordHash, nil, "PasswordHash")
    assert_eq(type(recorded.oauth), "table", "nested table kept")
    assert_eq(recorded.oauth.provider, "g", "nested value kept")
    assert_eq(recorded.oauth.tokens, nil, "nested tokens")
    assert_eq(recorded.oauth.claims, nil, "nested claims")
    assert_eq(recorded.oauth.nested.idToken, nil, "two levels down")
    assert_eq(recorded.oauth.nested.keep, 2, "two levels down kept")
    assert_eq(recorded.list[1].Secret, nil, "inside an array")
    assert_eq(recorded.list[1].keep, 1, "inside an array kept")
end)

test("session_audit44.scrub_is_depth_bounded", function()
    session.init({ cleanup = false })
    local recorded
    local cyc = {}
    cyc.self = cyc
    local h = session.login_handler(cookie, {
        audit_log = { record = function(_, _, _, o) recorded = o.metadata end },
        audit_metadata = function() return { factors = "pw", loop = cyc } end,
    })
    local ok, err = pcall(h, mkreq(), mkres(), { id = 45 })
    assert_eq(ok, true, tostring(err))
    assert_eq(recorded.factors, "pw", "recorded")
end)

test("session_audit44.undecodable_data_destroys_row", function()
    session.init({ cleanup = false })
    -- Data nested past the JSON decoder's depth ceiling: it encodes, but the
    -- stored text cannot be read back.
    local deep = {}
    local cur = deep
    for _ = 1, 300 do cur.n = {}; cur = cur.n end
    local sid = session.create({ user_id = 1, deep = deep })
    local ok, data = pcall(session.load, sid)
    assert_eq(ok, true, "load did not raise: " .. tostring(data))
    assert_eq(data, nil, "no session")
    assert_eq(session.update(sid, { user_id = 1 }), false, "the row is gone")
end)

-- ── auth ───────────────────────────────────────────────────────────

test("auth_audit44.optional_requires_literal_true", function()
    session.init({ cleanup = false })
    for _, v in ipairs({ "no", 1, "false", {} }) do
        local mw = auth.session_middleware({ optional = v })
        local res = mkres()
        assert_eq(mw(mkreq({ host = "app.test" }), res), 1, "session optional=" .. tostring(v))
        assert_eq(res.code, 401, "session 401")
        mw = auth.jwt_middleware({ secret = ("j"):rep(32), optional = v })
        res = mkres()
        assert_eq(mw(mkreq({ host = "app.test" }), res), 1, "jwt optional=" .. tostring(v))
        assert_eq(res.code, 401, "jwt 401")
    end
    assert_eq(auth.session_middleware({ optional = true })(mkreq(), mkres()), 0, "true")
    assert_eq(auth.jwt_middleware({ secret = ("j"):rep(32), optional = true })(mkreq(), mkres()),
              0, "jwt true")
end)

-- ── rbac ───────────────────────────────────────────────────────────

test("rbac_audit44.require_role_bad_uid", function()
    rbac.init()
    for _, bad in ipairs({ false, 5, "", {}, { "admin", 5 }, { "" } }) do
        assert_eq(pcall(rbac.require_role, bad), false, "role " .. tostring(bad))
        assert_eq(pcall(rbac.require_permission, bad), false, "perm " .. tostring(bad))
    end
    assert_eq(pcall(rbac.require_role, nil), false, "role nil")
    assert_eq(pcall(rbac.require_permission, nil), false, "perm nil")
    assert_eq(pcall(rbac.require_role, "admin"), true, "a name")
    assert_eq(pcall(rbac.require_role, { "admin", "editor" }), true, "a list")
end)

test("rbac_audit44.empty_user_id_is_401", function()
    rbac.init()
    rbac.define_role("a44admin", { "a44.read" })
    rbac.assign("44", "a44admin")
    local mws = { rbac.require_role("a44admin"), rbac.require_permission("a44.read") }
    for _, uid in ipairs({ 1.5, {}, true, "", -0.5 }) do
        for i, mw in ipairs(mws) do
            local res = mkres()
            local ok, rv = pcall(mw, mkreq(nil, { session = { user_id = uid } }), res)
            assert_eq(ok, true, "mw " .. i .. " uid " .. tostring(uid) .. ": " .. tostring(rv))
            assert_eq(rv, 1, "stopped")
            assert_eq(res.code, 401, "mw " .. i .. " uid " .. tostring(uid))
        end
    end
    local gmw = rbac.require_role("a44admin", { get_user_id = function() return 2.5 end })
    local res = mkres()
    assert_eq(gmw(mkreq(), res), 1, "get_user_id")
    assert_eq(res.code, 401, "get_user_id 401")
    for i, mw in ipairs(mws) do
        assert_eq(mw(mkreq(nil, { session = { user_id = 44 } }), mkres()), 0, "mw " .. i .. " holder")
    end
end)

-- ── auth-flows ─────────────────────────────────────────────────────

local AT = __test_hooks("hull.web.auth-flows")
local sent
local users = {}

local function af_init(extra)
    AT.reset()
    sent = {}
    local opts = {
        secret = ("s"):rep(32),
        trusted_hosts = { "app.example.com" },
        email_rate_limit = false,
        email_send = function(to, subject, _html, text)
            sent[#sent + 1] = { to = to, subject = subject, text = text }
        end,
        templates = {
            welcome        = function(c) return { subject = "w", text = c.verify_url } end,
            magic_link     = function(c) return { subject = "m", text = c.link } end,
            password_reset = function(c) return { subject = "p", text = c.link } end,
            email_change   = function(c) return { subject = "e", text = c.link } end,
            email_change_notify = function(c) return { subject = "n", text = c.revoke_url } end,
        },
        user_find_by_email = function(e) return users[e] end,
        user_get = function(id)
            for _, u in pairs(users) do if tostring(u.id) == tostring(id) then return u end end
        end,
        user_create = function() return nil end,
        user_set_password = function() end,
        user_set_email = function() end,
        user_set_email_verified = function() end,
        on_login = function(_req, res) res:json({ ok = true }) end,
        on_password_reset = function() end,
    }
    for k, v in pairs(extra or {}) do opts[k] = v end
    af.init(opts)
end

test("authflows_audit44.origin_host_case", function()
    users["ann@x.test"] = { id = 7, email = "ann@x.test", email_verified = true,
                            password_hash = crypto.hash_password("pw-ann-1234567") }
    af_init()
    for _, host in ipairs({ "App.Example.com", "APP.EXAMPLE.COM", "app.example.com" }) do
        assert_eq(AT.origin_for({ headers = { host = host } }), "https://app.example.com", host)
    end
    assert_eq(AT.origin_for({ headers = { host = "evil.example.com" } }), nil, "other host")
    -- The cross-site check and the link agree: the mail goes out.
    local req = mkreq({ host = "App.Example.com", ["content-type"] = "application/json",
                        ["sec-fetch-site"] = "same-origin" })
    req.body = json.encode({ email = "ann@x.test" })
    local res = mkres()
    AT.handlers.password_reset_request(req, res)
    assert_eq(#sent, 1, "reset mail sent")
    assert_eq(sent[1].text:sub(1, #"https://app.example.com/"), "https://app.example.com/",
              tostring(sent[1].text))
    users["ann@x.test"] = nil
end)

test("auth_flows_audit44.unknown_login_rows_reaped_by_cleanup", function()
    af_init({ cleanup = false })
    assert_eq(type(af.cleanup), "function", "auth-flows exports cleanup()")
    local old = AT.unknown_login_key("ghost-old@x.test")
    local fresh = AT.unknown_login_key("ghost-new@x.test")
    AT.bump_failed_login(old, 5)
    AT.bump_failed_login(fresh, 5)
    AT.backdate_login_attempt(old, 2 * 86400)
    assert_eq(AT.login_failed_count(old), 1, "old row there")
    af.cleanup()
    assert_eq(AT.login_failed_count(old), 0, "an old unknown-address row is reaped")
    assert_eq(AT.login_failed_count(fresh), 1, "a recent one is kept")
    local ok = pcall(af_init, { cleanup_at = 5 })
    assert_eq(ok, false, "cleanup_at must be a string")
end)

print(string.format("auth audit 44: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
