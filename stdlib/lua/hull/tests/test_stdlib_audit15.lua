-- test_stdlib_audit15.lua - stdlib regressions from audit 15 (Lua).
--
-- jobs.init upgrades a database from before its additive columns (the
-- harness creates old-schema _hull_jobs / _hull_cron tables first, and checks
-- the columns afterwards): the ALTERs ran through pcall(db.exec, ...), whose
-- C frame the _hull_* namespace check refused. A ttl - session, idempotency,
-- inbox, totp's lockout_duration_per_ip - is a positive finite NUMBER in both
-- runtimes; a string, 0, NaN or infinity raises. TOTP secrets are sealed
-- bound to their account ({"totp", user_id}); rows sealed before, unbound,
-- still open, and a blob bound to another account does not.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): everything here needs the db.

local jobs        = require("hull.jobs")
local session     = require("hull.web.middleware.session")
local idempotency = require("hull.web.middleware.idempotency")
local inbox       = require("hull.web.middleware.inbox")
local totp        = require("hull.web.middleware.totp")
local sealbox     = require("hull.crypto.sealbox")

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

local function raises(fn, needle, msg)
    local ok, err = pcall(fn)
    assert_eq(ok, false, (msg or "") .. ": raises")
    if needle then
        assert_eq(tostring(err):find(needle, 1, true) ~= nil, true,
                  (msg or "") .. ": message names " .. needle .. " (" .. tostring(err) .. ")")
    end
end

local BAD = { "86400", 0, -1, 0 / 0, math.huge, true }

-- ── jobs.init upgrades an old schema ───────────────────────────────

test("jobs.init adds the missing columns of an old schema", function()
    local ok, err = pcall(jobs.init)
    assert_eq(ok, true, "jobs.init: " .. tostring(err))
    local id = jobs.enqueue("audit15", { n = 1 })
    assert_eq(id ~= nil, true, "enqueue on the upgraded table")
    local job = jobs.get(id)
    assert_eq(job and job.progress, 0, "the added progress column is read")
    assert_eq((pcall(jobs.init)), true, "a second init is a no-op")
end)

-- ── ttl options are numbers ────────────────────────────────────────

test("session: ttl / absolute_ttl / per-call ttl must be positive numbers", function()
    for _, bad in ipairs(BAD) do
        raises(function() session.init({ ttl = bad }) end, "ttl", "init ttl " .. tostring(bad))
        raises(function() session.init({ absolute_ttl = bad }) end, "absolute_ttl",
               "init absolute_ttl " .. tostring(bad))
    end
    session.init({ ttl = 60, absolute_ttl = false })
    session.init({ ttl = 60, absolute_ttl = 3600 })
    local sid = session.create({ user_id = "u1" })
    assert_eq(type(sid), "string")
    for _, bad in ipairs(BAD) do
        raises(function() session.create({}, { ttl = bad }) end, "ttl", "create " .. tostring(bad))
        raises(function() session.load(sid, { ttl = bad }) end, "ttl", "load " .. tostring(bad))
        raises(function() session.update(sid, {}, { ttl = bad }) end, "ttl", "update " .. tostring(bad))
    end
    assert_eq(session.load(sid, { ttl = 120 }).user_id, "u1", "a number ttl works")
end)

test("idempotency: init and middleware ttl must be positive numbers", function()
    for _, bad in ipairs(BAD) do
        raises(function() idempotency.init({ ttl = bad }) end, "ttl", "init " .. tostring(bad))
        raises(function() idempotency.middleware({ ttl = bad }) end, "ttl", "mw " .. tostring(bad))
    end
    idempotency.init({ ttl = 60 })
    assert_eq(type(idempotency.middleware({ ttl = 30 })), "function")
end)

test("inbox: init and per-call ttl must be positive numbers", function()
    for _, bad in ipairs(BAD) do
        raises(function() inbox.init({ ttl = bad }) end, "ttl", "init " .. tostring(bad))
        raises(function() inbox.mark_processed("m1", "a15", { ttl = bad }) end, "ttl",
               "mark " .. tostring(bad))
        raises(function() inbox.check_and_mark("m2", "a15", { ttl = bad }) end, "ttl",
               "check " .. tostring(bad))
    end
    inbox.init({ ttl = 60 })
    assert_eq(inbox.check_and_mark("m3", "a15", { ttl = 5 }), false, "new")
    assert_eq(inbox.check_and_mark("m3", "a15", { ttl = 5 }), true, "duplicate")
end)

test("totp: lockout_duration_per_ip must be a positive number", function()
    for _, bad in ipairs(BAD) do
        __test_hooks('hull.web.middleware.totp').reset()
        raises(function() totp.init({ lockout_duration_per_ip = bad }) end,
               "lockout_duration_per_ip", tostring(bad))
    end
    __test_hooks('hull.web.middleware.totp').reset()
    totp.init({ lockout_duration_per_ip = 60 })
end)

-- ── TOTP secrets are bound to their account ────────────────────────

local KEY = ("k"):rep(32)

test("totp: a new seal is bound to its account; an old unbound one still opens", function()
    __test_hooks('hull.web.middleware.totp').reset()
    totp.init({ issuer = "T", encryption_key = KEY })
    local secret = ("S"):rep(20)
    local blob, flag, version = __test_hooks('hull.web.middleware.totp').encrypt_secret(secret, "u1")
    assert_eq(flag, 1); assert_eq(version, 1)
    local pt, v, unbound = __test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1, "u1")
    assert_eq(pt, secret, "opens for its account")
    assert_eq(v, 1)
    assert_eq(unbound, nil, "bound")
    assert_eq(__test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1, "u2"), nil,
              "a blob bound to u1 does not open for u2")
    -- A row sealed before audit 15, with no context.
    local ring = sealbox.keyring({ keys = { [1] = KEY }, current = 1 })
    local legacy = sealbox.seal(ring, secret)
    pt, v, unbound = __test_hooks('hull.web.middleware.totp').decrypt_secret(legacy, 1, "u1")
    assert_eq(pt, secret, "an unbound row still opens")
    assert_eq(v, 1)
    assert_eq(unbound, true, "flagged for a re-seal")
end)

test("totp: enroll + confirm store a bound secret", function()
    __test_hooks('hull.web.middleware.totp').reset()
    totp.init({ issuer = "T", encryption_key = KEY })
    local r = totp.enroll("u15")
    local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32)
    local code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, __test_hooks('hull.web.middleware.totp').current_step(), 6)
    assert_eq(totp.confirm("u15", code), true, "confirm")
    local blob, enc = __test_hooks('hull.web.middleware.totp').get_blob("u15")
    assert_eq(enc, 1)
    local pt, _, unbound = __test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1, "u15")
    assert_eq(pt, secret, "opens for its account")
    assert_eq(unbound, nil, "sealed bound")
    assert_eq(__test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1, "u16"), nil, "not for another")
end)

print(string.format("stdlib audit 15: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
