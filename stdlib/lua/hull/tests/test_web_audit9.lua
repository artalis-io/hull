-- test_web_audit9.lua - web stdlib regressions from audit 9 (Lua).
--
-- auth-flows: an undone (confirmed, then revoked) email change restores the
-- old address's verified state, makes the password unusable and pauses new
-- changes (M-a, M-b); _hull_auth_* rows are keyed by the text form of the id
-- (M-c); a confirm whose new address was taken since answers 409 without
-- consuming the link; a pending-2FA token dies with the password it was
-- issued against. Plus ratelimit (a spent bucket survives eviction), cookie
-- (first occurrence wins, values decoded as JS decodes them), session logout
-- provenance, the idempotency default principal and the oauth tid check.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): auth-flows and session need the db.

local json      = require("hull.json")
local crypto    = require("hull.crypto")
local af        = require("hull.web.auth-flows")
local ratelimit = require("hull.web.middleware.ratelimit")
local cache     = require("hull.cache")
local cookie    = require("hull.web.cookie")
local session   = require("hull.web.middleware.session")
local idem      = require("hull.web.middleware.idempotency")
local oauth     = require("hull.web.middleware.oauth")

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

-- ── Fakes ──────────────────────────────────────────────────────────

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

local function mkreq(body, ctx, headers)
    local h = { host = "app.test", ["content-type"] = "application/json",
                ["sec-fetch-site"] = "same-origin" }
    for k, v in pairs(headers or {}) do h[k] = v end
    return { method = "POST", path = "/auth/x", headers = h,
             body = json.encode(body or {}), ctx = ctx or {},
             remote_addr = "192.0.2.7" }
end

-- An in-memory user store keyed by the TEXT of the id, holding INTEGER ids
-- (the M-c shape: an app whose users table has an INTEGER PRIMARY KEY).
local S
local function reset_store()
    S = { by_id = {}, by_email = {}, sent = {}, resets = 0, logins = 0 }
end
local function add_user(id, email, pw, verified)
    local u = { id = id, email = email, email_verified = verified,
                password_hash = pw and crypto.hash_password(pw) or nil }
    S.by_id[tostring(id)] = u
    S.by_email[email] = u
    return u
end

local function init(extra)
    af._test.reset()
    local opts = {
        secret = ("s"):rep(32),
        trust_request_host = true,
        email_rate_limit = false,
        email_send = function(to, subject, _html, text)
            S.sent[#S.sent + 1] = { to = to, subject = subject, text = text }
        end,
        templates = {
            welcome        = function(c) return { subject = "w", text = c.verify_url } end,
            magic_link     = function(c) return { subject = "m", text = c.link } end,
            password_reset = function(c) return { subject = "p", text = c.link } end,
            email_change   = function(c) return { subject = "e", text = c.link } end,
            email_change_notify = function(c) return { subject = "n", text = c.revoke_url } end,
        },
        user_find_by_email = function(e) return S.by_email[e] end,
        user_get = function(id) return S.by_id[tostring(id)] end,
        user_create = function(e, h)
            local id = 100
            while S.by_id[tostring(id)] do id = id + 1 end
            local u = { id = id, email = e, password_hash = h, email_verified = false }
            S.by_id[tostring(id)] = u; S.by_email[e] = u
            return id
        end,
        user_set_password = function(id, h) S.by_id[tostring(id)].password_hash = h end,
        user_set_email = function(id, e)
            local u = S.by_id[tostring(id)]
            S.by_email[u.email] = nil
            u.email = e
            S.by_email[e] = u
        end,
        user_set_email_verified = function(id, v) S.by_id[tostring(id)].email_verified = v end,
        on_login = function(_req, res, user)
            S.logins = S.logins + 1
            res:json({ ok = true, id = user.id })
        end,
        on_password_reset = function() S.resets = S.resets + 1 end,
        email_change_reauth = function() return true end,
    }
    for k, v in pairs(extra or {}) do opts[k] = v end
    af.init(opts)
end

local H = af._test.handlers

local function token_in(to)
    for i = #S.sent, 1, -1 do
        if S.sent[i].to == to then return (S.sent[i].text:match("token=(.+)$")) end
    end
    return nil
end

-- Start an email change for user `id` to `new_email`; returns confirm and
-- revoke tokens.
local function start_change(id, old_email, new_email)
    local res = mkres()
    H.email_change(mkreq({ new_email = new_email }, { user_id = id }), res)
    assert_eq(res.body and res.body.ok, true, "email-change start")
    return token_in(new_email), token_in(old_email)
end

-- ── auth-flows ─────────────────────────────────────────────────────

test("uid_key: integer ids are keyed as text", function()
    assert_eq(af._test.uid_key(5), "5")
    assert_eq(af._test.uid_key("u1"), "u1")
end)

test("undo of a confirmed change: unverified old address stays unverified, "
     .. "password unusable, new changes paused (M-a, M-b)", function()
    reset_store(); init()
    local u = add_user(5, "old5@x.test", "first-password-1", false)
    local old_hash = u.password_hash
    local ctok, rtok = start_change(5, "old5@x.test", "new5@x.test")
    assert_eq(ctok ~= nil and rtok ~= nil, true, "tokens mailed")
    local res = mkres()
    H.email_change_confirm(mkreq({ token = ctok }), res)
    assert_eq(res.code, 200, "confirm")
    assert_eq(u.email, "new5@x.test")
    assert_eq(u.email_verified, true)
    res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.body and res.body.restored, true, "restored")
    assert_eq(res.body.password_reset_required, true)
    assert_eq(u.email, "old5@x.test")
    assert_eq(u.email_verified, false, "the old address was not verified")
    assert_eq(u.password_hash ~= old_hash, true, "password replaced")
    assert_eq(crypto.verify_password("first-password-1", u.password_hash), false,
              "the old password no longer works")
    assert_eq(S.resets >= 1, true, "sessions revoked")
    -- The cooldown row refuses a new change.
    res = mkres()
    H.email_change(mkreq({ new_email = "again5@x.test" }, { user_id = 5 }), res)
    assert_eq(res.code, 409, "paused")
    assert_eq(tostring(res.body.error):find("paused", 1, true) ~= nil, true)
end)

test("undo of a confirmed change: a verified old address stays verified (M-a)", function()
    reset_store(); init()
    local u = add_user(6, "old6@x.test", "first-password-1", true)
    local ctok, rtok = start_change(6, "old6@x.test", "new6@x.test")
    H.email_change_confirm(mkreq({ token = ctok }), mkres())
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.body.restored, true)
    assert_eq(u.email, "old6@x.test")
    assert_eq(u.email_verified, true)
end)

test("cancel of a pending change keeps the password and allows a new one", function()
    reset_store(); init()
    local u = add_user(8, "old8@x.test", "first-password-1", true)
    local old_hash = u.password_hash
    local _, rtok = start_change(8, "old8@x.test", "new8@x.test")
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.body.restored, false)
    assert_eq(u.password_hash, old_hash)
    res = mkres()
    H.email_change(mkreq({ new_email = "other8@x.test" }, { user_id = 8 }), res)
    assert_eq(res.body and res.body.ok, true, "a new change can start")
end)

test("confirm re-checks that the new address is still free (409, link kept)", function()
    reset_store(); init()
    local u = add_user(7, "old7@x.test", "first-password-1", true)
    local ctok = start_change(7, "old7@x.test", "taken7@x.test")
    local squatter = add_user(70, "taken7@x.test", "other-password-1", false)
    local res = mkres()
    H.email_change_confirm(mkreq({ token = ctok }), res)
    assert_eq(res.code, 409, "address in use")
    assert_eq(u.email, "old7@x.test")
    -- The address is freed again: the same link still works.
    S.by_email[squatter.email] = nil
    S.by_id["70"] = nil
    res = mkres()
    H.email_change_confirm(mkreq({ token = ctok }), res)
    assert_eq(res.code, 200, "confirm after the address was freed")
    assert_eq(u.email, "taken7@x.test")
end)

test("a pending-2FA token dies with the password it was issued against", function()
    reset_store()
    init({ enable_totp = true,
           user_totp_enrolled = function() return true end,
           totp_verify = function() return true end })
    local u = add_user(9, "u9@x.test", "first-password-1", true)
    local res = mkres()
    H.login(mkreq({ email = "u9@x.test", password = "first-password-1" }), res)
    local tok = res.body and res.body.totp_token
    assert_eq(type(tok), "string", "pending token")
    u.password_hash = crypto.hash_password("second-password-2")
    res = mkres()
    H.totp_verify(mkreq({ token = tok, code = "123456" }), res)
    assert_eq(res.code, 400, "old pending token refused")
    assert_eq(S.logins, 0)
    res = mkres()
    H.login(mkreq({ email = "u9@x.test", password = "second-password-2" }), res)
    tok = res.body.totp_token
    res = mkres()
    H.totp_verify(mkreq({ token = tok, code = "123456" }), res)
    assert_eq(res.body and res.body.ok, true, "fresh token works")
    assert_eq(S.logins, 1)
end)

-- ── ratelimit ──────────────────────────────────────────────────────

test("ratelimit: a spent bucket (count == limit) survives eviction", function()
    local buckets = cache.new({ max_entries = 2 })
    local sat = { map = {}, n = 0 }
    local now = 1000
    assert_eq(ratelimit.check(buckets, "A", 2, 60, now, sat).allowed, true)
    assert_eq(ratelimit.check(buckets, "A", 2, 60, now, sat).allowed, true)
    ratelimit.check(buckets, "B", 2, 60, now, sat)
    ratelimit.check(buckets, "C", 2, 60, now, sat)
    assert_eq(ratelimit.check(buckets, "A", 2, 60, now, sat).allowed, false,
              "A's spent allowance came back after eviction")
end)

-- ── cookie ─────────────────────────────────────────────────────────

test("cookie: first occurrence wins; values decode as in JS", function()
    local c = cookie.parse("sid=first; sid=second; q=\"x%20y\"; bad=%zz; hi=%FF")
    assert_eq(c.sid, "first")
    assert_eq(c.q, "x y")
    assert_eq(c.bad, "%zz")
    assert_eq(c.hi, "%FF", "invalid UTF-8 stays as sent")
    local set = cookie.serialize("n", "a b/c", { secure = false })
    assert_eq(set:find("n=a%20b%2Fc", 1, true) ~= nil, true, set)
    local back = cookie.parse(set:match("^([^;]*)"))
    assert_eq(back.n, "a b/c")
end)

-- ── session logout ─────────────────────────────────────────────────

test("session.logout_handler: same-site and foreign Origin refused", function()
    session.init({ cleanup = false })
    local lo = session.logout_handler(cookie)
    local function run(headers)
        local res = mkres()
        lo({ method = "POST", headers = headers }, res)
        return res.code
    end
    assert_eq(run({ host = "app.test", ["sec-fetch-site"] = "same-site" }), 403)
    assert_eq(run({ host = "app.test", ["sec-fetch-site"] = "cross-site" }), 403)
    assert_eq(run({ host = "app.test", origin = "https://evil.test" }), 403)
    assert_eq(run({ host = "app.test", referer = "https://evil.test/x" }), 403)
    assert_eq(run({ host = "app.test", origin = "null" }), 403)
    assert_eq(run({ host = "app.test", ["sec-fetch-site"] = "same-origin" }), 200)
    assert_eq(run({ host = "app.test", origin = "http://app.test" }), 200)
    assert_eq(run({ host = "app.test" }), 200, "a header-less client passes")
end)

-- ── idempotency ────────────────────────────────────────────────────

test("idempotency: session and JWT principals are prefixed apart", function()
    local p = idem._default_principal
    assert_eq(p({ ctx = { session = { user_id = 5 } } }), "session:5")
    assert_eq(p({ ctx = { user = { sub = 5 } } }), "user:5")
    assert_eq(p({ ctx = { session = { user_id = "user:5" } } }) ~= "user:5", true)
    assert_eq(p({ ctx = {} }), "__anon")
end)

-- ── oauth ──────────────────────────────────────────────────────────

test("oauth: a multi-tenant issuer must be the token's own tenant", function()
    local ok = oauth._test.issuer_ok
    local common = oauth._test.presets.microsoft({ tenant = "common" })
    local iss = "https://login.microsoftonline.com/1111-aaaa/v2.0"
    assert_eq(ok(common, { iss = iss, tid = "1111-aaaa" }), true)
    assert_eq(ok(common, { iss = iss, tid = "2222-bbbb" }), false, "other tid")
    assert_eq(ok(common, { iss = iss }), false, "no tid")
    local orgs = oauth._test.presets.microsoft({ tenant = "organizations" })
    local msa = "https://login.microsoftonline.com/9188040d-6c67-4c5b-b112-36a304b66dad/v2.0"
    assert_eq(ok(orgs, { iss = msa, tid = "9188040d-6c67-4c5b-b112-36a304b66dad" }), false)
    local pinned = { issuer = "https://idp.test" }
    assert_eq(ok(pinned, { iss = "https://idp.test" }), true)
end)

print(string.format("web audit 9: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
