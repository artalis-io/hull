-- Async HTTP Client — Hull + Lua example
--
-- Run: hull app.lua -p 3000 --no-sandbox
-- Test: curl localhost:3000/health
--       curl localhost:3000/sleep
--       curl localhost:3000/async-fetch
--
-- Demonstrates hull.sleep() and http.fetch() — both yield the
-- coroutine and let the event loop serve other connections while
-- waiting. Compare /sync-fetch (blocks event loop) vs /async-fetch
-- (non-blocking via KlWatcher).

-- Allow outbound HTTP to self (localhost) for the fetch demos
app.manifest({
    hosts = {"127.0.0.1"},
})

-- ── Routes ─────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Mock slow API endpoint (uses async sleep internally)
app.get("/api/slow", function(_req, res)
    hull.sleep(50)
    res:json({ value = 42, source = "mock" })
end)

-- Sync HTTP: blocks the event loop while waiting for response
app.get("/sync-fetch", function(req, res)
    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local resp = http.get("http://127.0.0.1:" .. port .. "/api/slow")
    res:json({ mode = "sync", status = resp.status, body = resp.body })
end)

-- Async HTTP: yields coroutine, event loop stays responsive
app.get("/async-fetch", function(req, res)
    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local resp = http.fetch("GET", "http://127.0.0.1:" .. port .. "/api/slow")
    res:json({ mode = "async", status = resp.status, body = resp.body })
end)

-- Async POST with body and headers
app.get("/async-post", function(req, res)
    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local resp = http.fetch("POST", "http://127.0.0.1:" .. port .. "/echo", {
        body = '{"greeting":"hello"}',
        headers = {["Content-Type"] = "application/json"},
    })
    res:json({ mode = "async", status = resp.status, body = resp.body })
end)

-- Echo body back as JSON (target for async-post)
app.post("/echo", function(req, res)
    res:json({ body = req.body })
end)

-- Sleep demo (existing async primitive)
app.get("/sleep", function(_req, res)
    hull.sleep(100)
    res:json({ slept = true })
end)

log.info("Async HTTP example loaded — routes registered")
