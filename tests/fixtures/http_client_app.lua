-- http_client_app.lua — E2E test app for HTTP client capability
--
-- Declares hosts manifest to enable the http module,
-- then exercises all HTTP methods against an echo server.
--
-- Expects echo server running on 127.0.0.1:19860
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

app.manifest({
    hosts = {"127.0.0.1"},
    env = {"ECHO_PORT"},
})

local ECHO_BASE = "http://127.0.0.1:19860"

-- GET /test/get — exercise http.get()
app.get("/test/get", function(_req, res)
    local r = http.get(ECHO_BASE .. "/echo")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/post — exercise http.post()
app.get("/test/post", function(_req, res)
    local r = http.post(ECHO_BASE .. "/echo", "hello from lua", {
        headers = { ["X-Test"] = "lua-post" }
    })
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/put — exercise http.put()
app.get("/test/put", function(_req, res)
    local r = http.put(ECHO_BASE .. "/echo", "put-body")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/patch — exercise http.patch()
app.get("/test/patch", function(_req, res)
    local r = http.patch(ECHO_BASE .. "/echo", "patch-body")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/delete — exercise http.delete()
app.get("/test/delete", function(_req, res)
    local r = http.delete(ECHO_BASE .. "/echo")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/request — exercise http.request() with custom method
app.get("/test/request", function(_req, res)
    local r = http.request("OPTIONS", ECHO_BASE .. "/echo")
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/headers — verify custom headers are sent
app.get("/test/headers", function(_req, res)
    local r = http.get(ECHO_BASE .. "/echo", {
        headers = { ["X-Custom-Header"] = "test-value-lua" }
    })
    res:json({ status = r.status, echo = r.body })
end)

-- GET /test/denied — verify host not in allowlist is rejected
app.get("/test/denied", function(_req, res)
    local ok, err = pcall(function()
        http.get("http://evil.example.com/steal")
    end)
    if ok then
        res:json({ error = "should have been denied" })
    else
        res:json({ denied = true, message = tostring(err) })
    end
end)

-- Health check for readiness
app.get("/health", function(_req, res)
    res:json({ status = "ok", runtime = "lua" })
end)

log.info("HTTP client test app loaded (Lua)")
