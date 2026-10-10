-- test_auth_audit12.lua - auth-flows regressions from audit 12 (Lua).
--
-- Token mails (magic link, password reset, verify resend, and the
-- send_password_reset / send_magic_link helpers) go to the account's STORED
-- address, and only when the typed address is that address (the domain may
-- differ in ASCII case): an app lookup that folds case or accents no longer
-- hands an account's token to whoever reads a lookalike mailbox; the bundled
-- standard_users lookup compares exactly. The email-change notice skips the
-- per-recipient rate limit and a change whose notice cannot be sent is not
-- started. The recovery lock of an undo that could not restore the old
-- address no longer expires (unlock_recovery clears it). The revoke and the
-- confirm act on the change's row only while it is still the state they read.
-- init refuses email_change_notify without on_password_reset and enable_totp
-- without totp_disable.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): auth-flows needs the db.

local json   = require("hull.json")
local crypto = require("hull.crypto")
local af     = require("hull.web.auth-flows")
local db     = require("hull.db").default()

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

local function mkreq(body, ctx)
    return { method = "POST", path = "/auth/x",
             headers = { host = "app.test", ["content-type"] = "application/json",
                         ["sec-fetch-site"] = "same-origin" },
             body = json.encode(body or {}), ctx = ctx or {},
             remote_addr = "192.0.2.7" }
end

-- The store's lookup FOLDS, as a MySQL utf8mb4_0900_ai_ci column does: case
-- and the accents these tests use.
local function fold(e)
    return (e:lower():gsub("\xc3\xa9", "e"):gsub("\xc3\x89", "e"))
end

local S
local function reset_store()
    S = { by_id = {}, by_email = {}, sent = {}, resets = 0, totp_off = {},
          order = {}, fail_notice = nil, fail_set_email = nil }
end
local function add_user(id, email, pw, verified)
    local u = { id = id, email = email, email_verified = verified,
                password_hash = pw and crypto.hash_password(pw) or nil }
    S.by_id[tostring(id)] = u
    S.by_email[fold(email)] = u
    return u
end
local function count(t) local n = 0; for _ in pairs(t) do n = n + 1 end; return n end

local function init(extra)
    af._test.reset()
    local opts = {
        secret = ("s"):rep(32),
        trust_request_host = true,
        email_rate_limit = false,
        email_change_ttl = 86400,
        email_send = function(to, subject, _html, text)
            if subject == "n" and S.fail_notice == "throw" then error("smtp down") end
            if subject == "n" and S.fail_notice == "false" then return false end
            S.sent[#S.sent + 1] = { to = to, subject = subject, text = text }
        end,
        templates = {
            welcome        = function(c) return { subject = "w", text = c.verify_url } end,
            magic_link     = function(c) return { subject = "m", text = c.link } end,
            password_reset = function(c) return { subject = "p", text = c.link } end,
            email_change   = function(c) return { subject = "e", text = c.link } end,
            email_change_notify = function(c) return { subject = "n", text = c.revoke_url } end,
        },
        user_find_by_email = function(e) return S.by_email[fold(e)] end,
        user_get = function(id)
            if S.on_get then local f = S.on_get; S.on_get = nil; f() end
            return S.by_id[tostring(id)]
        end,
        user_create = function(e, h)
            local id = 100
            while S.by_id[tostring(id)] do id = id + 1 end
            local u = { id = id, email = e, password_hash = h, email_verified = false }
            S.by_id[tostring(id)] = u; S.by_email[fold(e)] = u
            return id
        end,
        user_set_password = function(id, h) S.by_id[tostring(id)].password_hash = h end,
        user_set_email = function(id, e)
            if S.fail_set_email == e then error("duplicate key") end
            local u = S.by_id[tostring(id)]
            S.by_email[fold(u.email)] = nil
            u.email = e
            S.by_email[fold(e)] = u
        end,
        user_set_email_verified = function(id, v) S.by_id[tostring(id)].email_verified = v end,
        on_login = function(_req, res) res:json({ ok = true }) end,
        on_password_reset = function()
            S.resets = S.resets + 1
            S.order[#S.order + 1] = "sessions"
        end,
        totp_disable = function(id)
            S.totp_off[#S.totp_off + 1] = id
            S.order[#S.order + 1] = "totp"
            S.locked_at_totp = af._test.recovery_locked(id)
        end,
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

local function start_change(id, new_email)
    local res = mkres()
    H.email_change(mkreq({ new_email = new_email }, { user_id = id }), res)
    return res
end

-- A confirmed change of account `id` from old_email to new_email; returns
-- the revoke token.
local function confirmed_change(id, old_email, new_email)
    local res = start_change(id, new_email)
    assert_eq(res.body and res.body.ok, true, "email-change start")
    local ctok, rtok = token_in(new_email), token_in(old_email)
    res = mkres()
    H.email_change_confirm(mkreq({ token = ctok }), res)
    assert_eq(res.code, 200, "confirm")
    return rtok
end

-- ── H1: token mails go to the stored address ──────────────────────

test("same_address: equal after ASCII lowercasing, nothing more", function()
    local sa = af._test.same_address
    assert_eq(sa("jose@x.test", "jose@x.test"), true)
    assert_eq(sa("jose@X.Test", "jose@x.test"), true, "domain case")
    assert_eq(sa("JOSE@x.test", "jose@x.test"), true, "local-part ASCII case")
    assert_eq(sa("jos\xc3\xa9@x.test", "jose@x.test"), false, "accent")
    assert_eq(sa("jose@x.t\xc3\xa9st", "jose@x.test"), false, "non-ASCII domain")
    assert_eq(sa("jose@x.test", nil), false, "no stored address")
end)

test("reset / magic link / helpers for a lookalike address mail nothing", function()
    reset_store(); init({ magic_link_auto_signup = true })
    add_user(1, "jose@x.test", "first-password-1", true)
    for _, typed in ipairs({ "jos\xc3\xa9@x.test", "JOS\xc3\x89@x.test", "jose@x.t\xc3\xa9st" }) do
        local res = mkres()
        H.password_reset_request(mkreq({ email = typed }), res)
        assert_eq(res.body and res.body.ok, true, "generic ok")
        res = mkres()
        H.magic_link(mkreq({ email = typed }), res)
        assert_eq(res.body and res.body.ok, true, "generic ok")
        af.send_password_reset(typed, "https://app.test")
        af.send_magic_link(typed, "https://app.test")
    end
    assert_eq(#S.sent, 0, "no token mailed, to either address")
    assert_eq(count(S.by_id), 1, "no account auto-created for a lookalike")
end)

test("a token mail goes to the stored address, not the typed one", function()
    reset_store(); init()
    add_user(2, "ann@x.test", "first-password-1", true)
    H.password_reset_request(mkreq({ email = "ann@X.TEST" }), mkres())
    H.magic_link(mkreq({ email = "ann@X.TEST" }), mkres())
    af.send_password_reset("ann@X.Test", "https://app.test")
    af.send_magic_link("ann@X.Test", "https://app.test")
    assert_eq(sent_to("ann@x.test"), 4, "all four to the stored address")
    assert_eq(sent_to("ann@X.TEST") + sent_to("ann@X.Test"), 0, "none to the typed")
end)

test("verify resend: lookalike mails nothing; the stored address gets it", function()
    reset_store(); init()
    add_user(3, "eve@x.test", "first-password-1", false)
    H.verify_resend(mkreq({ email = "\xc3\x89ve@x.test" }), mkres())
    H.verify_resend(mkreq({ email = "\xd0\xb5ve@x.test" }), mkres())
    assert_eq(#S.sent, 0, "nothing for a lookalike")
    H.verify_resend(mkreq({ email = "EVE@X.test" }), mkres())
    assert_eq(sent_to("eve@x.test"), 1, "an ASCII case variant: to the stored address")
    assert_eq(sent_to("EVE@X.test"), 0, "never to the typed spelling")
end)

test("standard_users: ASCII case-insensitive, accents exact, legacy rows found", function()
    reset_store(); init()
    db.exec("DROP TABLE IF EXISTS au12_users")
    db.exec("CREATE TABLE au12_users (id TEXT PRIMARY KEY, "
            .. "email TEXT NOT NULL UNIQUE COLLATE NOCASE, password_hash TEXT, "
            .. "email_verified INTEGER NOT NULL DEFAULT 0, "
            .. "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)")
    local users = af.standard_users({ table = "au12_users" })
    local id = users.create("Alice@x.test", "h")
    local stored = db.query("SELECT email FROM au12_users WHERE id = ?", { id })
    assert_eq(stored[1].email, "alice@x.test", "stored ASCII-lowercased")
    assert_eq(users.find_by_email("alice@x.test").id, id, "lowercase finds it")
    assert_eq(users.find_by_email("ALICE@X.TEST").id, id, "ASCII case variant finds it")
    assert_eq(users.find_by_email("alic\xc3\xa9@x.test"), nil, "accent variant does not")
    db.exec("INSERT INTO au12_users (id, email, password_hash, email_verified, "
            .. "created_at, updated_at) VALUES ('legacy', 'Bob@X.test', 'h', 0, 0, 0)")
    local legacy = users.find_by_email("bob@x.test")
    assert_eq(legacy and legacy.id, "legacy", "a row stored with ASCII capitals is still found")
    db.exec("DROP TABLE au12_users")
end)

-- ── M: the email-change notice ─────────────────────────────────────

test("the notice to the old address is not rate limited", function()
    reset_store(); init({ email_rate_limit = { limit = 3, window = 900 } })
    add_user(10, "old10@x.test", "first-password-1", true)
    -- Anyone can fill the old address's bucket with anonymous requests.
    for _ = 1, 3 do H.magic_link(mkreq({ email = "old10@x.test" }), mkres()) end
    S.sent = {}
    local res = start_change(10, "new10@x.test")
    assert_eq(res.body and res.body.ok, true)
    assert_eq(sent_to("old10@x.test"), 1, "notice sent anyway")
    assert_eq(sent_to("new10@x.test"), 1, "confirm mail sent")
end)

test("a change whose notice cannot be sent is not started", function()
    for _, mode in ipairs({ "throw", "false" }) do
        reset_store(); init()
        local u = add_user(11, "old11@x.test", "first-password-1", true)
        S.fail_notice = mode
        local res = start_change(11, "new11@x.test")
        assert_eq(res.code, 503, mode .. ": 503")
        assert_eq(sent_to("new11@x.test"), 0, mode .. ": no confirm mail")
        S.fail_notice = nil
        res = start_change(11, "new11@x.test")
        assert_eq(res.body and res.body.ok, true, mode .. ": no pending row left behind")
        res = mkres()
        H.email_change_confirm(mkreq({ token = token_in("new11@x.test") }), res)
        assert_eq(u.email, "new11@x.test", mode .. ": the retried change confirms")
    end
end)

-- ── M: the recovery lock persists ──────────────────────────────────

test("the recovery lock does not expire; unlock_recovery clears it", function()
    reset_store(); init()
    add_user(20, "old20@x.test", "first-password-1", true)
    local rtok = confirmed_change(20, "old20@x.test", "new20@x.test")
    add_user(21, "old20@x.test", "squatter-pw-1", true)
    -- The undo writes its lock with an expiry already past.
    init({ email_change_ttl = -100 })
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.code, 409, "cannot restore")
    init()
    af._test.gc_expired()
    assert_eq(af._test.recovery_locked(20), true, "still locked after expiry + gc")
    S.sent = {}
    H.password_reset_request(mkreq({ email = "new20@x.test" }), mkres())
    assert_eq(sent_to("new20@x.test"), 0, "no reset while locked")
    res = start_change(20, "other20@x.test")
    assert_eq(res.code, 409, "changes paused while locked")
    assert_eq(af.unlock_recovery(20), true, "cleared")
    assert_eq(af.unlock_recovery(20), false, "nothing left to clear")
    H.password_reset_request(mkreq({ email = "new20@x.test" }), mkres())
    assert_eq(sent_to("new20@x.test"), 1, "reset after the operator unlocks")
end)

test("the lock row is written before totp_disable; sessions go first", function()
    reset_store(); init()
    add_user(22, "old22@x.test", "first-password-1", true)
    local rtok = confirmed_change(22, "old22@x.test", "new22@x.test")
    add_user(23, "old22@x.test", "squatter-pw-1", true)
    H.email_change_revoke(mkreq({ token = rtok }), mkres())
    assert_eq(S.locked_at_totp, true, "locked when totp_disable ran")
    assert_eq(S.order[1], "sessions")
    assert_eq(S.order[2], "totp")
end)

-- ── M: a restore the database refuses falls back to the lock ───────

test("an undo whose restore the database refuses locks recovery", function()
    reset_store(); init()
    local u = add_user(30, "old30@x.test", "first-password-1", true)
    local rtok = confirmed_change(30, "old30@x.test", "new30@x.test")
    S.fail_set_email = "old30@x.test"
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.code, 409, "nothing to restore to after all")
    assert_eq(u.email, "new30@x.test")
    assert_eq(af._test.recovery_locked(30), true, "recovery locked")
    assert_eq(crypto.verify_password("first-password-1", u.password_hash), false,
              "password made unusable")
    assert_eq(#S.totp_off, 1, "second factor dropped")
end)

-- ── Lows ───────────────────────────────────────────────────────────

test("a revoke whose pending change is confirmed meanwhile undoes it", function()
    reset_store(); init()
    local u = add_user(40, "old40@x.test", "first-password-1", true)
    local res = start_change(40, "new40@x.test")
    assert_eq(res.body and res.body.ok, true)
    local ctok, rtok = token_in("new40@x.test"), token_in("old40@x.test")
    -- The confirm lands (another instance) between the revoke's read of the
    -- pending row and its delete.
    S.on_get = function()
        H.email_change_confirm(mkreq({ token = ctok }), mkres())
    end
    res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(u.email, "old40@x.test", "the confirmed change was undone")
    assert_eq(res.body and res.body.restored, true)
end)

test("a confirm whose address switch fails leaves no pending row", function()
    reset_store(); init()
    local u = add_user(41, "old41@x.test", "first-password-1", true)
    start_change(41, "new41@x.test")
    S.fail_set_email = "new41@x.test"
    local ok = pcall(H.email_change_confirm,
                     mkreq({ token = token_in("new41@x.test") }), mkres())
    assert_eq(ok, false, "the failure propagates")
    S.fail_set_email = nil
    local res = start_change(41, "again41@x.test")
    assert_eq(res.body and res.body.ok, true, "row deleted, not put back to pending")
    assert_eq(u.email, "old41@x.test")
end)

test("an unreadable email_verified is held for the undo, restored unverified", function()
    reset_store(); init()
    local u = add_user(42, "old42@x.test", "first-password-1", "1")
    local rtok = confirmed_change(42, "old42@x.test", "new42@x.test")
    assert_eq(af.email_reserved("old42@x.test"), true, "held")
    local res = mkres()
    H.email_change_revoke(mkreq({ token = rtok }), res)
    assert_eq(res.body and res.body.restored, true)
    assert_eq(u.email, "old42@x.test")
    assert_eq(u.email_verified, false, "restored as unverified")
end)

test("init refuses email_change_notify without on_password_reset", function()
    reset_store()
    local ok, err = pcall(init, { on_password_reset = false })
    assert_eq(ok, false)
    assert_eq(tostring(err):find("on_password_reset", 1, true) ~= nil, true)
end)

test("init refuses enable_totp without totp_disable", function()
    reset_store()
    local ok, err = pcall(init, { enable_totp = true, totp_disable = false,
                                  user_totp_enrolled = function() return false end,
                                  totp_verify = function() return false end })
    assert_eq(ok, false)
    assert_eq(tostring(err):find("totp_disable", 1, true) ~= nil, true)
end)

print(string.format("auth audit 12: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
