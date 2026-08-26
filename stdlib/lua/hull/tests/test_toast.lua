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
    -- toast.success(res, msg, { level = "error" }) - shorthand wins
    local res = mock_res()
    toast.success(res, "x", { level = "error" })
    assert_eq(decoded(res).toast.level, "success")
end)

-- ── Wire-payload type narrowing (M2 from the audit) ───────────────

test("show drops non-number duration silently", function()
    -- Documented contract: duration is ms-number. A caller passing
    -- a string shouldn't make it onto the wire (would confuse the
    -- client-side setTimeout call).
    local res = mock_res()
    toast.show(res, "x", { duration = "8000" })
    local d = decoded(res)
    if d.toast.duration ~= nil then
        error("non-number duration should not reach the wire payload")
    end
end)

test("show coerces non-string id to string", function()
    -- A numeric id is acceptable but normalized to string so the
    -- client-side dedup Map keys consistently.
    local res = mock_res()
    toast.show(res, "x", { id = 42 })
    assert_eq(decoded(res).toast.id, "42")
end)

-- ── Composition: multiple HX-Trigger events on one response ───────

test("toast composes with other HX-Trigger events on the same response", function()
    -- The toast helper just sets HX-Trigger via htmx.trigger; a
    -- subsequent htmx.trigger call on the same response replaces
    -- the header (Hull's response API is last-write-wins for
    -- headers). The documented composition story is "use the
    -- multi-event form of htmx.trigger" - verify it works.
    local htmx = require("hull.web.htmx")
    local res = mock_res()
    toast.success(res, "saved")
    -- Then add a second event by reading the existing payload and
    -- emitting a combined trigger.
    local first = decoded(res)
    htmx.trigger(res, {
        toast = first.toast,
        ["asset-saved"] = { id = 42 },
    })
    local combined = decoded(res)
    assert_eq(combined.toast.level, "success")
    assert_eq(combined["asset-saved"].id, 42)
end)

print(string.format("\n%d/%d toast tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end
