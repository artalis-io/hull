-- test_stdlib_audit29.lua - stdlib regressions from audit 29 (Lua).
--
-- jobs: dedup_key / concurrency_key are stored as strings (an integer is
-- its decimal string), and keys / step / signal names over the VARCHAR(255)
-- column are refused. csrf: more than 256 form pairs is a 413, and an empty
-- token header falls back to the _csrf body field. auth: the Bearer scheme
-- is case-insensitive, spaces / tabs around the token are dropped, and an
-- empty token is a 401. The JS twin is test_stdlib_audit29.js.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): jobs needs the db.

local jobs = require("hull.jobs")
local csrf = require("hull.web.middleware.csrf")
local auth = require("hull.web.middleware.auth")
local jwt  = require("hull.jwt")
local time = require("hull.time")

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

local function refused(fn, ...)
    local ok = pcall(fn, ...)
    return not ok
end

local function mock_res()
    local res = { code = 200 }
    function res:status(c) self.code = c; return self end
    function res:json(b) self.body = b; return self end
    function res:header() return self end
    return res
end

-- ── jobs ─────────────────────────────────────────────────────────────

test("jobs.enqueue: an integer dedup_key is its decimal string", function()
    jobs.init()
    local id = jobs.enqueue("a29int", {}, { queue = "a29q", dedup_key = 7 })
    assert(id, "first enqueue returns an id")
    -- The same key as a string collapses onto it: one stored form.
    assert_eq(jobs.enqueue("a29int", {}, { queue = "a29q", dedup_key = "7" }), nil,
              "string twin collapsed")
    -- "07" is another key (a numeric compare would have matched it).
    local id2 = jobs.enqueue("a29int", {}, { queue = "a29q", dedup_key = "07" })
    assert(id2 and id2 ~= id, "\"07\" is a distinct key")
end)

test("jobs.enqueue: a non-string, non-integer key is refused", function()
    jobs.init()
    assert(refused(jobs.enqueue, "a29bad", {}, { dedup_key = {} }), "table")
    assert(refused(jobs.enqueue, "a29bad", {}, { dedup_key = 1.5 }), "float")
    assert(refused(jobs.enqueue, "a29bad", {}, { dedup_key = true }), "boolean")
    assert(refused(jobs.enqueue, "a29bad", {},
        { concurrency_key = {}, concurrency = 1 }), "concurrency_key table")
end)

test("jobs.enqueue: a key over 255 bytes is refused", function()
    jobs.init()
    assert(refused(jobs.enqueue, "a29long", {}, { dedup_key = string.rep("k", 256) }),
           "dedup_key 256")
    assert(refused(jobs.enqueue, "a29long", {},
        { concurrency_key = string.rep("k", 256), concurrency = 1 }), "concurrency_key 256")
    -- 86 x 3-byte characters = 258 bytes (counted in bytes, as the JS twin).
    assert(refused(jobs.enqueue, "a29long", {}, { dedup_key = string.rep("\u{20AC}", 86) }),
           "multi-byte over 255 bytes")
    assert(jobs.enqueue("a29long", {}, { queue = "a29l", dedup_key = string.rep("k", 255) }),
           "255 bytes fits")
end)

test("jobs.signal: a name over 200 bytes is refused", function()
    jobs.init()
    assert(refused(jobs.signal, 1, string.rep("s", 201)), "201")
end)

-- ── csrf ─────────────────────────────────────────────────────────────

local SECRET = "audit29-csrf-secret-0123456789abcdef"

test("csrf: more than 256 form pairs is a 413", function()
    local mw = csrf.middleware({ secret = SECRET })
    local parts = {}
    for i = 1, 257 do parts[i] = "f" .. i .. "=v" end
    local req = { method = "POST", headers = {}, ctx = { session_id = "s1" },
                  body = table.concat(parts, "&") }
    local res = mock_res()
    assert_eq(mw(req, res), 1, "short-circuit")
    assert_eq(res.code, 413, "status")
    -- Empty pieces count, as the JS twin's split counts them.
    req.body = "a=b" .. string.rep("&", 256)
    res = mock_res()
    assert_eq(mw(req, res), 1, "empty pieces")
    assert_eq(res.code, 413, "status (empty pieces)")
    -- 256 pairs is not over the cap.
    parts[257] = nil
    parts[256] = "_csrf=" .. csrf.generate("s1", SECRET)
    req.body = table.concat(parts, "&")
    res = mock_res()
    assert_eq(mw(req, res), 0, "256 pairs with the token at the end")
end)

test("csrf: an empty token header falls back to the body field", function()
    local mw = csrf.middleware({ secret = SECRET })
    local tok = csrf.generate("s2", SECRET)
    local req = { method = "POST", headers = { ["x-csrf-token"] = "" },
                  ctx = { session_id = "s2" }, body = "_csrf=" .. tok }
    local res = mock_res()
    assert_eq(mw(req, res), 0, "body token read")
    req = { method = "POST", headers = { ["x-csrf-token"] = "" },
            ctx = { session_id = "s2" }, body = "x=1" }
    res = mock_res()
    assert_eq(mw(req, res), 1, "no token")
    assert_eq(res.code, 403, "status")
end)

-- ── auth Bearer parsing ──────────────────────────────────────────────

test("auth.jwt_middleware: one Bearer rule", function()
    local secret = "audit29-jwt-secret-0123456789abcdef"
    local mw = auth.jwt_middleware({ secret = secret })
    local tok = jwt.sign({ sub = "u1", exp = time.now() + 600 }, secret)
    local function run(h)
        local req = { method = "GET", path = "/api", ctx = {},
                      headers = { authorization = h } }
        local res = mock_res()
        return mw(req, res), res.code
    end
    for _, h in ipairs({ "Bearer " .. tok, "bearer " .. tok, "BEARER " .. tok,
                         "Bearer\t" .. tok, "Bearer   " .. tok .. "  " }) do
        local rc = run(h)
        assert_eq(rc, 0, "accepted: " .. h:sub(1, 9))
    end
    for _, h in ipairs({ "Bearer ", "Bearer    ", "Bearer", "Bearer" .. tok,
                         "Basic " .. tok }) do
        local rc, code = run(h)
        assert_eq(rc, 1, "refused: " .. h:sub(1, 9))
        assert_eq(code, 401, "401: " .. h:sub(1, 9))
    end
end)

print(string.format("stdlib audit 29: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
