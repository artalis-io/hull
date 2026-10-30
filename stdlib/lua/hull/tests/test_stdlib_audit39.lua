-- test_stdlib_audit39.lua - stdlib regressions from audit 39 (Lua).
--
-- retry.run re-raised errors at level 1, which put "hull.retry:N: " in front
-- of them: a runtime refusal ("[hull:busy] ...") then no longer started with
-- its code, so jobs counted the attempt instead of handing the job back, and
-- an ordinary error gained a second position prefix. It now re-raises at
-- level 0, and a refusal is re-raised at once, never retried. The JS twin is
-- test_stdlib_audit39.js. (The jobs workflow and email provider cases are in
-- test_jobs_hooks.lua / test_email.lua.)
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c).

local retry = require("hull.retry")

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

for _, code in ipairs({ "busy", "wait_refused" }) do
    test("a " .. code .. " refusal is re-raised at once, unchanged", function()
        local calls = 0
        local msg = "[hull:" .. code .. "] http.fetch: refused"
        local ok, err = pcall(retry.run, function()
            calls = calls + 1
            error(msg, 0)
        end, { max_attempts = 5 })
        assert_eq(ok, false, "raised")
        assert_eq(err, msg, "prefix kept")
        assert_eq(calls, 1, "not retried")
    end)
end

test("an exhausted error keeps its text", function()
    local ok, err = pcall(retry.run, function() error("plain failure", 0) end,
                          { max_attempts = 1 })
    assert_eq(ok, false, "raised")
    assert_eq(err, "plain failure", "no hull.retry position prefix")
end)

test("a permanent error (retry_on_error false) keeps its text", function()
    local ok, err = pcall(retry.run, function() error("permanent", 0) end,
                          { max_attempts = 3, retry_on_error = function() return false end })
    assert_eq(ok, false, "raised")
    assert_eq(err, "permanent", "no hull.retry position prefix")
end)

test("a table error passes through as the same value", function()
    local e = { code = "x" }
    local ok, err = pcall(retry.run, function() error(e) end, { max_attempts = 1 })
    assert_eq(ok, false, "raised")
    assert_eq(err, e, "same table")
end)

print(pass .. " passed, " .. fail .. " failed")
return { pass = pass, fail = fail }
