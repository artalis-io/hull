-- test_toast.lua. Tests for hull.web.htmx.toast.
--
-- Verifies the helper sets the HX-Trigger header to the expected
-- JSON payload shape. Doesn't exercise the client-side rendering
-- (separate concern; covered by e2e_htmx_widgets.sh against a
-- running server).

local toast = require('hull.web.htmx.toast')
local json  = require('hull.json')

local pass, fail = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name .. ": " .. tostring(err)) end
end
local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

local function mock_res()
    local headers = {}
    return {
        headers_set = headers,
        header = function(self, name, value) headers[name] = value end,
    }
end

local function decoded(res)
    local v = res.headers_set["HX-Trigger"]
    if not v then return nil end
    return json.decode(v)
end

test("show emits HX-Trigger with default level info", function()
    local res = mock_res()
    toast.show(res, "hello")
    local d = decoded(res)
    assert_eq(d.toast.message, "hello")
    assert_eq(d.toast.level, "info")
end)

test("show coerces unknown level to info", function()
    local res = mock_res()
    toast.show(res, "x", { level = "marquee" })
    assert_eq(decoded(res).toast.level, "info")
end)

test("success shorthand emits level=success", function()
    local res = mock_res()
    toast.success(res, "saved")
    assert_eq(decoded(res).toast.level, "success")
end)

test("error shorthand emits level=error", function()
    local res = mock_res()
    toast.error(res, "denied")
    assert_eq(decoded(res).toast.level, "error")
end)

test("warning shorthand emits level=warning", function()
    local res = mock_res()
    toast.warning(res, "deprecated")
    assert_eq(decoded(res).toast.level, "warning")
end)

test("info shorthand emits level=info", function()
    local res = mock_res()
    toast.info(res, "fyi")
    assert_eq(decoded(res).toast.level, "info")
end)

test("show passes duration through", function()
    local res = mock_res()
    toast.show(res, "x", { duration = 8000 })
    assert_eq(decoded(res).toast.duration, 8000)
end)

test("show passes id through", function()
    local res = mock_res()
    toast.show(res, "x", { id = "perm-denied" })
    assert_eq(decoded(res).toast.id, "perm-denied")
end)

test("show coerces nil message to empty string", function()
    local res = mock_res()
    toast.show(res, nil)
    assert_eq(decoded(res).toast.message, "")
end)

test("shorthand opts override level if provided", function()
    -- toast.success(res, msg, { level = "error" }) — shorthand wins
    local res = mock_res()
    toast.success(res, "x", { level = "error" })
    assert_eq(decoded(res).toast.level, "success")
end)

print(string.format("\n%d/%d toast tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end
