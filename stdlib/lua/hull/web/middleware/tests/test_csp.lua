-- test_csp.lua. Tests for hull.web.middleware.csp

local csp = require('hull.web.middleware.csp')

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

local function assert_true(v, msg)
    if not v then error(msg or "expected truthy value") end
end

local function assert_contains(haystack, needle, msg)
    if not haystack or not string.find(haystack, needle, 1, true) then
        error((msg or "") .. " expected '" .. needle ..
              "' in: " .. tostring(haystack))
    end
end

local function assert_not_contains(haystack, needle, msg)
    if haystack and string.find(haystack, needle, 1, true) then
        error((msg or "") .. " expected '" .. needle ..
              "' NOT in: " .. tostring(haystack))
    end
end

-- Mock request + response. Mirror the parts the middleware uses.
local function mock_req()
    return { ctx = {} }
end

local function mock_res()
    local headers = {}
    local status_code
    local body
    return {
        headers_set = headers,
        header = function(self, name, value) headers[name] = value end,
        status = function(self, code) status_code = code end,
        json = function(self, data) body = data end,
        get_status = function() return status_code end,
        get_body = function() return body end,
    }
end

-- ── nonce() helper ───────────────────────────────────────────────────

test("nonce() returns a non-empty base64url string", function()
    local n = csp.nonce()
    assert_true(n ~= nil, "nonce should not be nil")
    assert_true(#n >= 20, "128-bit base64url should be 22 chars")
    -- base64url chars only
    assert_eq(string.match(n, "^[A-Za-z0-9_-]+$") ~= nil, true,
              "nonce must match base64url alphabet")
end)

test("two nonces are different (RNG sanity)", function()
    local n1 = csp.nonce()
    local n2 = csp.nonce()
    assert_true(n1 ~= n2, "successive nonces must differ")
end)

-- ── csp.htmx() - Pico-compatible profile ─────────────────────────────

test("htmx() sets CSP header and exposes nonce in ctx", function()
    local mw = csp.htmx()
    local req, res = mock_req(), mock_res()
    local rc = mw(req, res)
    assert_eq(rc, 0, "middleware should pass through (return 0)")
    assert_true(req.ctx.csp_nonce ~= nil, "ctx.csp_nonce must be set")
    local hdr = res.headers_set["Content-Security-Policy"]
    assert_true(hdr ~= nil, "CSP header must be set")
    assert_contains(hdr, "default-src 'self'")
    assert_contains(hdr, "script-src 'self' 'nonce-")
    assert_contains(hdr, "style-src 'self' 'nonce-")
    -- the Pico concession:
    assert_contains(hdr, "style-src-attr 'unsafe-inline'")
    assert_contains(hdr, "frame-ancestors 'none'")
    assert_contains(hdr, "base-uri 'self'")
    -- nonce in header must match the one stored in ctx
    assert_contains(hdr, "'nonce-" .. req.ctx.csp_nonce .. "'",
                    "header nonce must equal ctx.csp_nonce")
end)

test("htmx() generates a fresh nonce per request", function()
    local mw = csp.htmx()
    local req1, res1 = mock_req(), mock_res()
    local req2, res2 = mock_req(), mock_res()
    mw(req1, res1)
    mw(req2, res2)
    assert_true(req1.ctx.csp_nonce ~= req2.ctx.csp_nonce,
                "per-request nonces must differ")
end)

-- ── csp.strict() - no inline-style escape ────────────────────────────

test("strict() omits style-src-attr 'unsafe-inline'", function()
    local mw = csp.strict()
    local req, res = mock_req(), mock_res()
    mw(req, res)
    local hdr = res.headers_set["Content-Security-Policy"]
    assert_true(hdr ~= nil)
    assert_contains(hdr, "style-src 'self' 'nonce-")
    assert_not_contains(hdr, "style-src-attr",
                        "strict profile must not include style-src-attr")
end)

-- ── report_only ──────────────────────────────────────────────────────

test("report_only=true uses CSP-Report-Only header", function()
    local mw = csp.htmx({ report_only = true })
    local req, res = mock_req(), mock_res()
    mw(req, res)
    assert_true(res.headers_set["Content-Security-Policy-Report-Only"] ~= nil,
                "Report-Only header must be set")
    assert_eq(res.headers_set["Content-Security-Policy"], nil,
              "regular CSP header must NOT be set in report-only mode")
end)

print(string.format("hull.web.middleware.csp: %d passed, %d failed", pass, fail))
if fail > 0 then error("csp tests failed") end
