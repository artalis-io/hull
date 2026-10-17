-- test_stdlib_audit17.lua - stdlib regressions from audit 17 (Lua).
--
-- etag.text / etag.html with an empty body: etag.compute returns nil, and
-- res:header("ETag", nil) raised (a 500). An empty body now goes out with no
-- ETag and is never answered 304. jobs._config (the live config, settable
-- past jobs.init's checks) is a test hook, no longer on the module.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): etag hashes, jobs needs the db.

local etag = require("hull.web.middleware.etag")
local jobs = require("hull.jobs")

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

local function mock_res(helper)
    local res = { code = 200, sent = false, headers = {} }
    function res:status(code) self.code = code; return self end
    function res:header(name, value)
        if value == nil then error("header value is nil") end
        self.headers[name] = value
        return self
    end
    res[helper] = function(self) self.sent = true; return self end
    return res
end

-- ── etag: an empty body ─────────────────────────────────────────────

for _, helper in ipairs({ "text", "html" }) do
    for _, body in ipairs({ "", false }) do
        local label = helper .. (body == "" and " empty" or " nil") .. " body"
        test(label .. " is sent with no ETag", function()
            local res = mock_res(helper)
            etag[helper]({ method = "GET", headers = {} }, res, body or nil)
            assert_eq(res.sent, true, "sent")
            assert_eq(res.code, 200, "status")
            assert_eq(res.headers.ETag, nil, "no ETag")
        end)
        test(label .. " is never a 304, even for If-None-Match: *", function()
            local res = mock_res(helper)
            etag[helper]({ method = "GET", headers = { ["if-none-match"] = "*" } },
                         res, body or nil)
            assert_eq(res.sent, true, "sent")
            assert_eq(res.code, 200, "status")
        end)
    end
end

test("a non-empty body still gets its ETag", function()
    local res = mock_res("text")
    etag.text({ method = "GET", headers = {} }, res, "hello")
    assert_eq(res.sent, true, "sent")
    assert_eq(type(res.headers.ETag), "string", "ETag")
end)

-- ── jobs: no exported config ────────────────────────────────────────

test("jobs._config is not exported", function()
    assert_eq(jobs._config, nil)
end)

test("the config is a test hook", function()
    local hooks = __test_hooks("hull.jobs")
    assert_eq(type(hooks and hooks.config), "table", "hook")
    assert_eq(type(hooks.config.backoff), "function", "live config")
end)

return { pass = pass, fail = fail }
