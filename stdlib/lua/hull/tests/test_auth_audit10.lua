-- test_auth_audit10.lua - auth stdlib regressions from audit 10 (Lua).
--
-- auth-flows: while a confirmed email change can still be undone, its old
-- address counts as taken (register, magic-link auto-signup, another
-- account's change request and confirm), so the undo still has an address to
-- restore; the undo whose address was taken anyway still makes the password
-- unusable and pauses changes; the undo removes a second factor through
-- totp_disable; magic-link auto-signup creates the account after the
-- response. Plus the logout provenance origins auth-flows registers, the
-- idempotency principal normalization and the inbox source check. (Deferred
-- work under db.batch runs after the transaction, as a hull._task: covered
-- with a real loop in tests/hull/runtime/lua/test_lua.c, lua_task.*.)
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): auth-flows and session need the db.

local json      = require("hull.json")
local crypto    = require("hull.crypto")
local af        = require("hull.web.auth-flows")
local cookie    = require("hull.web.cookie")
local session   = require("hull.web.middleware.session")
local idem      = require("hull.web.middleware.idempotency")
local inbox     = require("hull.web.middleware.inbox")

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

local S
local function reset_store()
    S = { by_id = {}, by_email = {}, sent = {}, resets = 0, totp_off = {} }
end
local function add_user(id, email, pw, verified)
    local u = { id = id, email = email, email_verified = verified,
                password_hash = pw and crypto.hash_password(pw) or nil }
    S.by_id[tostring(id)] = u
    S.by_email[email] = u
    return u
end
local function count(t) local n = 0; for _ in pairs(t) do n = n + 1 end; return n end

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
        on_login = function(_req, res) res:json({ ok = true }) end,
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
local function sent_to(to)
    local n = 0
    for _, m in ipairs(S.sent) do if m.to == to then n = n + 1 end end
    return n
end

-- A confirmed change of account `id` from old_email to new_email; returns
-- the revoke token.
local function confirmed_change(id, old_email, new_email)
    local res = mkres()
    H.email_change(mkreq({ new_email = new_email }, { user_id = id }), res)
    assert_eq(res.body and res.body.ok, true, "email-change start")
    local ctok, rtok = token_in(new_email), token_in(old_email)
    res = mkres()
    H.email_change_confirm(mkreq({ token = ctok }), res)
    assert_eq(res.code, 200, "confirm")
    return rtok
end

-- ── auth-flows: the vacated address is reserved ────────────────────

test("register of a vacated old address creates nothing; the undo restores it", function()
    reset_store(); init()
    local u = add_user(5, "old5@x.test", "first-password-1", true)
    local rtok = confirmed_change(5, "old5@x.test", "new5@x.test")
    S.sent = {}
    local res = mkres()
    H.register(mkreq({ email = "OLD5@x.test", password = "squatter-pw-1" }), res)
    assert_eq(res.body and res.body.ok, true, "same ok answer (case-insensitive)")
    res = mkres()
    H.register(mkreq({ email = "old5@x.test", password = "squatter-pw-1" }), res)
    assert_eq(res.body and res.body.ok, true, "same ok answer")
    assert_eq(S.by_email["old5@x.test"], nil, "no account created")
    assert_eq(count(S.by_id), 1, "one account")
    assert_eq(#S.sent, 0, "no welcome mail")
    res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.body and res.body.restored, true, "restored")
    assert_eq(u.email, "old5@x.test")
end)

test("magic-link auto-signup of a vacated address creates nothing", function()
    reset_store(); init({ magic_link_auto_signup = true })
    add_user(6, "old6@x.test", "first-password-1", true)
    confirmed_change(6, "old6@x.test", "new6@x.test")
    S.sent = {}
    local res = mkres()
    H.magic_link(mkreq({ email = "old6@x.test" }), res)
    assert_eq(res.body and res.body.ok, true)
    assert_eq(S.by_email["old6@x.test"], nil, "no account created")
    assert_eq(#S.sent, 0, "no mail")
    af.send_magic_link("old6@x.test", "https://app.test")
    assert_eq(S.by_email["old6@x.test"], nil, "send_magic_link creates nothing either")
end)

test("magic-link auto-signup creates a new account (after the response)", function()
    reset_store(); init({ magic_link_auto_signup = true })
    local res = mkres()
    H.magic_link(mkreq({ email = "fresh@x.test" }), res)
    assert_eq(res.body and res.body.ok, true)
    local u = S.by_email["fresh@x.test"]
    assert_eq(type(u), "table", "account created")
    assert_eq(u.password_hash, nil, "passwordless")
    assert_eq(sent_to("fresh@x.test"), 1, "magic link mailed")
    -- A second request finds it and creates no other.
    H.magic_link(mkreq({ email = "fresh@x.test" }), mkres())
    assert_eq(count(S.by_id), 1)
end)

test("another account cannot request or confirm a change to the vacated address", function()
    reset_store(); init()
    add_user(7, "old7@x.test", "first-password-1", true)
    local other = add_user(8, "b8@x.test", "other-password-1", true)
    -- Account 8 starts a change to old7 BEFORE account 7's change: its link
    -- is out when the address is vacated.
    local res = mkres()
    H.email_change(mkreq({ new_email = "old7@x.test" }, { user_id = 8 }), res)
    assert_eq(res.code, 409, "taken while account 7 holds it")
    confirmed_change(7, "old7@x.test", "new7@x.test")
    res = mkres()
    H.email_change(mkreq({ new_email = "old7@x.test" }, { user_id = 8 }), res)
    assert_eq(res.code, 409, "the vacated address is reserved")
    assert_eq(other.email, "b8@x.test")
end)

test("a confirm link to the vacated address answers 409", function()
    reset_store(); init()
    add_user(9, "old9@x.test", "first-password-1", true)
    local other = add_user(10, "b10@x.test", "other-password-1", true)
    -- Account 10 asks for c9@x.test while it is free; account 9 then takes
    -- it and moves on, leaving it vacated - and reserved for 9's undo.
    local res = mkres()
    H.email_change(mkreq({ new_email = "c9@x.test" }, { user_id = 10 }), res)
    assert_eq(res.body and res.body.ok, true)
    local ctok10 = token_in("c9@x.test")
    S.by_email["old9@x.test"] = nil
    S.by_id["9"].email = "c9@x.test"
    S.by_email["c9@x.test"] = S.by_id["9"]
    confirmed_change(9, "c9@x.test", "new9@x.test")
    assert_eq(S.by_email["c9@x.test"], nil, "c9 vacated")
    res = mkres()
    H.email_change_confirm(mkreq({ token = ctok10 }), res)
    assert_eq(res.code, 409, "reserved for the undo")
    assert_eq(other.email, "b10@x.test")
end)

test("undo whose address was taken anyway still resets the password and pauses changes", function()
    reset_store(); init()
    local u = add_user(11, "old11@x.test", "first-password-1", true)
    local old_hash = u.password_hash
    local rtok = confirmed_change(11, "old11@x.test", "new11@x.test")
    -- The app created an account on the address itself.
    add_user(12, "old11@x.test", "squatter-pw-1", true)
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.code, 409, "nothing to restore to")
    assert_eq(u.email, "new11@x.test")
    assert_eq(u.password_hash ~= old_hash, true, "password replaced")
    assert_eq(crypto.verify_password("first-password-1", u.password_hash), false)
    assert_eq(S.resets >= 1, true, "sessions revoked")
    res = mkres()
    H.email_change(mkreq({ new_email = "again11@x.test" }, { user_id = 11 }), res)
    assert_eq(res.code, 409, "paused")
end)

test("undo of a confirmed change removes the second factor via totp_disable", function()
    reset_store()
    init({ totp_disable = function(id) S.totp_off[#S.totp_off + 1] = id end })
    add_user(13, "old13@x.test", "first-password-1", true)
    local rtok = confirmed_change(13, "old13@x.test", "new13@x.test")
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.body and res.body.restored, true)
    assert_eq(#S.totp_off, 1, "totp_disable called")
    assert_eq(tostring(S.totp_off[1]), "13")
end)

-- ── logout provenance ──────────────────────────────────────────────

test("session logout trusts the origins auth-flows registered", function()
    reset_store()
    init({ trust_request_host = false, public_origin = "https://app.example.com",
           trusted_hosts = nil })
    session.init({ cleanup = false })
    local lo = session.logout_handler(cookie)
    local function run(headers)
        local res = mkres()
        lo({ method = "POST", headers = headers }, res)
        return res.code
    end
    -- Behind a proxy: Host is the upstream name, Origin the public one.
    assert_eq(run({ host = "10.0.0.5:8080", origin = "https://app.example.com" }), 200)
    assert_eq(run({ host = "10.0.0.5:8080", origin = "https://evil.test" }), 403)
    local lo2 = session.logout_handler(cookie, { trusted_hosts = { "alt.example.com" } })
    local res = mkres()
    lo2({ method = "POST", headers = { host = "10.0.0.5", origin = "https://alt.example.com:8443" } }, res)
    assert_eq(res.code, 200, "opts.trusted_hosts, any port")
    af._test.reset()
    assert_eq(run({ host = "10.0.0.5:8080", origin = "https://app.example.com" }), 403,
              "cleared with the auth-flows state")
end)

-- ── idempotency ────────────────────────────────────────────────────

test("idempotency: principals are text, capped at the column width", function()
    local n = idem._norm_principal
    assert_eq(n(5), "5")
    assert_eq(n(nil), "__anon")
    assert_eq(n(""), "__anon")
    assert_eq(n("user:5"), "user:5")
    local long = ("a"):rep(300)
    local h = n(long)
    assert_eq(#h <= 255, true)
    assert_eq(h:sub(1, 7), "sha256:")
    assert_eq(n(long .. "b") ~= h, true, "distinct long principals stay distinct")
    assert_eq(n(h) ~= h, true, "a literal hashed spelling cannot pose as one")
    assert_eq(pcall(n, {}), false, "a table is refused")
end)

-- ── inbox ──────────────────────────────────────────────────────────

test("inbox: an integer source is its decimal string; other types refused", function()
    inbox.init({ ttl = 60 })
    assert_eq(inbox.check_and_mark("m1", 42), false)
    assert_eq(inbox.check_and_mark("m1", "42"), true, "42 and \"42\" are one source")
    assert_eq(pcall(inbox.is_duplicate, "m1", {}), false)
    assert_eq(pcall(inbox.is_duplicate, "m1", ("s"):rep(256)), false, "over-length")
end)

-- ── audit 11 ───────────────────────────────────────────────────────

test("audit 11: an address unverified at confirm is not reserved", function()
    reset_store(); init()
    add_user(20, "old20@x.test", "first-password-1", false)
    confirmed_change(20, "old20@x.test", "new20@x.test")
    assert_eq(af.email_reserved("old20@x.test"), false)
    H.register(mkreq({ email = "old20@x.test", password = "owner-password-1" }), mkres())
    assert_eq(type(S.by_email["old20@x.test"]), "table", "the owner can register it")
end)

test("audit 11: email_reserved reports an address held for an undo", function()
    reset_store(); init()
    add_user(21, "old21@x.test", "first-password-1", true)
    confirmed_change(21, "old21@x.test", "new21@x.test")
    assert_eq(af.email_reserved("OLD21@x.test"), true)
    assert_eq(af.email_reserved("other21@x.test"), false)
end)

test("audit 11: an undo that cannot restore locks recovery and drops the second factor", function()
    reset_store()
    init({ totp_disable = function(id) S.totp_off[#S.totp_off + 1] = id end })
    add_user(22, "old22@x.test", "first-password-1", true)
    local rtok = confirmed_change(22, "old22@x.test", "new22@x.test")
    -- A magic link to the address the change set, issued before the undo.
    H.magic_link(mkreq({ email = "new22@x.test" }), mkres())
    local mtok = token_in("new22@x.test")
    assert_eq(type(mtok), "string", "magic link issued")
    add_user(23, "old22@x.test", "squatter-pw-1", true)
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.code, 409)
    assert_eq(#S.totp_off, 1, "totp_disable called")
    S.sent = {}
    H.password_reset_request(mkreq({ email = "new22@x.test" }), mkres())
    H.magic_link(mkreq({ email = "new22@x.test" }), mkres())
    af.send_password_reset("new22@x.test", "https://app.test")
    af.send_magic_link("new22@x.test", "https://app.test")
    assert_eq(sent_to("new22@x.test"), 0, "no reset or magic link while locked")
    res = mkres()
    H.magic_link_consume(mkreq({ token = mtok }), res)
    assert_eq(res.code, 400, "an earlier magic link no longer signs in")
end)

test("audit 11: idempotency skips a request a custom get_principal cannot place", function()
    for _, p in ipairs({ false, "" }) do
        local mw = idem.middleware({ get_principal = function() return p end })
        local req = { method = "POST", path = "/x", body = "{}",
                      headers = { ["idempotency-key"] = "k1" } }
        assert_eq(mw(req, mkres()), 0, "runs without idempotency")
    end
    local mw = idem.middleware({ get_principal = function() return nil end })
    assert_eq(mw({ method = "POST", path = "/x", body = "{}",
                   headers = { ["idempotency-key"] = "k1" } }, mkres()), 0)
end)

print(string.format("auth audit 10: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
