-- test_stdlib_audit28.lua - stdlib regressions from audit 28 (Lua).
--
-- template: safe_url percent-encodes a backslash (browsers read it as "/"
-- in an http(s) URL, so "/\evil.test" was the protocol-relative
-- "//evil.test"). The totp per-(ip, user) accounting is in
-- test_totp_audit28.lua.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c).

local template = require("hull.template")

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

test("safe_url: a backslash is percent-encoded", function()
    assert_eq(template.render_string("{{{ u | safe_url }}}", { u = "/\\evil.test" }),
              "/%5Cevil.test", "relative")
    assert_eq(template.render_string("{{{ u | safe_url }}}",
              { u = "https://x.test\\@evil.test/" }),
              "https://x.test%5C@evil.test/", "https")
    assert_eq(template.render_string("{{ u | safe_url }}", { u = "/a\\b" }),
              "/a%5Cb", "escaped output")
end)

print(string.format("stdlib audit 28: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
