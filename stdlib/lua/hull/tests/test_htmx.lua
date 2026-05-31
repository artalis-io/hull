-- test_htmx.lua. Tests for hull.web.htmx
--
-- Pure-function helpers; no runtime globals beyond hull.json (used
-- internally by the HX-Trigger encoders).

local htmx = require('hull.web.htmx')

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
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

-- A mock response object that records header() calls. Mirrors the
-- subset of the Keel response API the htmx helpers use.
local function mock_res()
    local headers = {}
    local status_code
    local body
    return {
        headers_set = headers,
        header = function(self, name, value) headers[name] = value end,
        status = function(self, code) status_code = code end,
        send = function(self, s) body = s end,
        redirect = function(self, path) headers["__redirect_to"] = path end,
        get_status = function() return status_code end,
        get_body = function() return body end,
    }
end

-- ── Request-inspection helpers ───────────────────────────────────────

test("is() returns true for HX-Request: true", function()
    local req = { headers = { ["hx-request"] = "true" } }
    assert_eq(htmx.is(req), true)
end)

test("is() returns false when header absent", function()
    assert_eq(htmx.is({ headers = {} }), false)
end)

test("is() handles nil req gracefully", function()
    assert_eq(htmx.is(nil), false)
    assert_eq(htmx.is({}), false)
end)

test("boosted() returns true for HX-Boosted: true", function()
    local req = { headers = { ["hx-boosted"] = "true" } }
    assert_eq(htmx.boosted(req), true)
end)

test("current_url() returns header value", function()
    local req = { headers = { ["hx-current-url"] = "https://example.com/x" } }
    assert_eq(htmx.current_url(req), "https://example.com/x")
end)

test("target() and trigger_name()", function()
    local req = { headers = {
        ["hx-target"] = "#todo-list",
        ["hx-trigger-name"] = "save-button",
    }}
    assert_eq(htmx.target(req), "#todo-list")
    assert_eq(htmx.trigger_name(req), "save-button")
end)

-- ── Response-header helpers ──────────────────────────────────────────

test("retarget sets HX-Retarget", function()
    local res = mock_res()
    htmx.retarget(res, "#errors")
    assert_eq(res.headers_set["HX-Retarget"], "#errors")
end)

test("reswap sets HX-Reswap", function()
    local res = mock_res()
    htmx.reswap(res, "outerHTML")
    assert_eq(res.headers_set["HX-Reswap"], "outerHTML")
end)

test("refresh sets HX-Refresh: true", function()
    local res = mock_res()
    htmx.refresh(res)
    assert_eq(res.headers_set["HX-Refresh"], "true")
end)

test("push_url sets HX-Push-Url", function()
    local res = mock_res()
    htmx.push_url(res, "/items/42")
    assert_eq(res.headers_set["HX-Push-Url"], "/items/42")
end)

test("push_url(false) suppresses default push", function()
    local res = mock_res()
    htmx.push_url(res, false)
    assert_eq(res.headers_set["HX-Push-Url"], "false")
end)

test("replace_url sets HX-Replace-Url", function()
    local res = mock_res()
    htmx.replace_url(res, "/items/43")
    assert_eq(res.headers_set["HX-Replace-Url"], "/items/43")
end)

-- ── HX-Trigger encoders ──────────────────────────────────────────────

test("trigger with bare event name sends string value", function()
    local res = mock_res()
    htmx.trigger(res, "saved")
    assert_eq(res.headers_set["HX-Trigger"], "saved")
end)

test("trigger with event + payload encodes JSON object", function()
    local res = mock_res()
    htmx.trigger(res, "saved", { id = 42 })
    -- Order of JSON keys is implementation-defined; assert on substring
    local v = res.headers_set["HX-Trigger"]
    assert_eq(v ~= nil, true, "trigger should set header")
    assert_eq(string.find(v, '"saved"') ~= nil, true, "should contain event name")
    assert_eq(string.find(v, '"id"') ~= nil, true, "should contain payload key")
    assert_eq(string.find(v, '42') ~= nil, true, "should contain payload value")
end)

test("trigger with table encodes table directly", function()
    local res = mock_res()
    htmx.trigger(res, { saved = { id = 1 }, refresh = true })
    local v = res.headers_set["HX-Trigger"]
    assert_eq(string.find(v, '"saved"') ~= nil, true)
    assert_eq(string.find(v, '"refresh"') ~= nil, true)
end)

test("trigger_after_swap uses HX-Trigger-After-Swap", function()
    local res = mock_res()
    htmx.trigger_after_swap(res, "settled")
    assert_eq(res.headers_set["HX-Trigger-After-Swap"], "settled")
end)

test("trigger_after_settle uses HX-Trigger-After-Settle", function()
    local res = mock_res()
    htmx.trigger_after_settle(res, "done")
    assert_eq(res.headers_set["HX-Trigger-After-Settle"], "done")
end)

-- ── Location helpers ─────────────────────────────────────────────────

test("location with string path sends bare path", function()
    local res = mock_res()
    htmx.location(res, "/dashboard")
    assert_eq(res.headers_set["HX-Location"], "/dashboard")
end)

test("location with table encodes as JSON context", function()
    local res = mock_res()
    htmx.location(res, { path = "/x", target = "#main", swap = "outerHTML" })
    local v = res.headers_set["HX-Location"]
    assert_eq(string.find(v, '"/x"') ~= nil, true)
    assert_eq(string.find(v, '"#main"') ~= nil, true)
end)

-- ── Redirect (the dual-mode helper) ──────────────────────────────────

test("redirect on htmx request sets HX-Redirect + 204", function()
    local req = { headers = { ["hx-request"] = "true" } }
    local res = mock_res()
    htmx.redirect(req, res, "/after-login")
    assert_eq(res.headers_set["HX-Redirect"], "/after-login")
    assert_eq(res.get_status(), 204)
    assert_eq(res.get_body(), "")
end)

test("redirect on plain request falls back to res:redirect", function()
    local req = { headers = {} }
    local res = mock_res()
    htmx.redirect(req, res, "/after-login")
    assert_eq(res.headers_set["__redirect_to"], "/after-login")
    -- HX-Redirect must NOT be set on a non-htmx request
    assert_eq(res.headers_set["HX-Redirect"], nil)
end)

-- ── Done ─────────────────────────────────────────────────────────────

print(string.format("hull.web.htmx: %d passed, %d failed", pass, fail))
if fail > 0 then error("hull.web.htmx tests failed") end
