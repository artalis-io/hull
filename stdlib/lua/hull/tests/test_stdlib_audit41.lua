-- test_stdlib_audit41.lua - stdlib regressions from audit 41 (Lua).
--
-- search: a tokenize option must be a string (a table with __len / __concat
-- passed the checks and was spliced into the SQL through a second __concat).
-- oauth: a JWKS body that is not JSON is a failed fetch, not a raised error.
-- totp: recovery_codes is capped at 32. session: login_handler's audit
-- metadata scrub matches keys without case, and an audit_metadata that raises
-- no longer fails the login after the session was made. The JS twin is
-- test_stdlib_audit41.js. (The workflow step store cases are in
-- test_jobs_hooks.lua, the idempotency fingerprint in
-- web/middleware/tests/test_idempotency.lua.)
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c).

local search  = require("hull.search")
local totp    = require("hull.web.middleware.totp")
local session = require("hull.web.middleware.session")
local cookie  = require("hull.web.cookie")
local oauth   = require("hull.web.middleware.oauth")

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

-- ── search ─────────────────────────────────────────────────────────

test("search: a tokenize option that is not a string is refused", function()
    local n = 0
    local evil = setmetatable({}, {
        __len = function() return 9 end,
        __concat = function(a, b)
            n = n + 1
            if n == 1 then return "unicode61 " end
            return "x'); DROP TABLE t; --"
        end,
    })
    local ok, err = pcall(search.create_index, "a41tok", { "body" }, { tokenize = evil })
    assert_eq(ok, false, "refused")
    assert_eq(tostring(err):find("tokenize", 1, true) ~= nil, true, tostring(err))
    assert_eq(n, 0, "no metamethod ran")
    assert_eq(pcall(search.create_index, "a41tok", { "body" }, { tokenize = 7 }), false, "a number")
    assert_eq(pcall(search.create_index, "a41tok", { "body" }, { tokenize = "porter ascii" }),
              true, "a valid string")
end)

-- ── oauth ──────────────────────────────────────────────────────────

test("oauth: a JWKS body that is not JSON is a failed fetch", function()
    oauth.init({ secret = ("s"):rep(32),
                 providers = { g = { preset = "google", client_id = "cid" } } })
    local hc = __test_hooks("hull._test_hooks").real("hull.http-client")  -- the stdlib's table (audit 43)
    local real = hc.async.get
    hc.async.get = function() return { status = 200, body = "<html>not json</html>" } end
    local ok, by_kid, err = pcall(__test_hooks("hull.web.middleware.oauth").refresh_jwks, "g")
    hc.async.get = real
    assert_eq(ok, true, "did not raise: " .. tostring(by_kid))
    assert_eq(by_kid, nil, "no keys")
    assert_eq(err, "jwks: malformed response")
    __test_hooks("hull.web.middleware.oauth").reset()
end)

-- ── totp ───────────────────────────────────────────────────────────

test("totp: recovery_codes is capped at 32", function()
    local KEY = ("k"):rep(32)
    local T = __test_hooks("hull.web.middleware.totp")
    T.reset()
    local ok, err = pcall(totp.init, { issuer = "T", encryption_key = KEY, recovery_codes = 33 })
    assert_eq(ok, false, "33 refused")
    assert_eq(tostring(err):find("recovery_codes", 1, true) ~= nil, true, tostring(err))
    assert_eq(pcall(totp.init, { issuer = "T", encryption_key = KEY, recovery_codes = 1e9 }),
              false, "1e9 refused")
    T.reset()
    assert_eq(pcall(totp.init, { issuer = "T", encryption_key = KEY, recovery_codes = 32 }),
              true, "32 accepted")
    T.reset()
end)

-- ── session ────────────────────────────────────────────────────────

local function mkres()
    local r = { code = 200, headers = {} }
    function r.status(self, c) self.code = c; return self end
    function r.json(self, v) self.body = v; return self end
    function r.header(self, k, v) self.headers[k] = v; return self end
    return r
end
local function mkreq()
    return { method = "POST", path = "/login", headers = { host = "app.test" }, ctx = {},
             remote_addr = "192.0.2.7" }
end

test("session: the audit metadata scrub matches keys without case", function()
    session.init({ cleanup = false })
    local recorded
    local audit = { record = function(_, _, _, o) recorded = o.metadata end }
    local h = session.login_handler(cookie, {
        audit_log = audit,
        audit_metadata = function()
            return { Access_Token = "at", CLAIMS = { sub = "x" }, PassWord = "pw",
                     Secret = "s", factors = "pw" }
        end,
    })
    h(mkreq(), mkres(), { id = 41 })
    assert_eq(type(recorded), "table", "recorded")
    assert_eq(recorded.factors, "pw", "kept")
    assert_eq(recorded.Access_Token, nil, "Access_Token scrubbed")
    assert_eq(recorded.CLAIMS, nil, "CLAIMS scrubbed")
    assert_eq(recorded.PassWord, nil, "PassWord scrubbed")
    assert_eq(recorded.Secret, nil, "Secret scrubbed")
end)

test("session: an audit_metadata that raises does not fail the login", function()
    session.init({ cleanup = false })
    local called = false
    local h = session.login_handler(cookie, {
        audit_log = { record = function() called = true end },
        audit_metadata = function() error("metadata exploded") end,
    })
    local res = mkres()
    local ok, err = pcall(h, mkreq(), res, { id = 42 })
    assert_eq(ok, true, "login answered: " .. tostring(err))
    assert_eq(res.body and res.body.ok, true, "the default respond ran")
    assert_eq(type(res.headers["Set-Cookie"]), "string", "the cookie went out")
    assert_eq(called, false, "no audit row with no metadata")
end)

print(string.format("stdlib audit 41: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
