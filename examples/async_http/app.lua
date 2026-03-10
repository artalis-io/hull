-- Async HTTP Client — Hull + Lua example
--
-- Run: hull app.lua -p 3000 --no-sandbox
-- Test: curl localhost:3000/health
--       curl localhost:3000/sleep
--       curl localhost:3000/async-fetch
--       curl localhost:3000/async-db
--       curl localhost:3000/worker-dispatch
--
-- Demonstrates hull.sleep(), http.async.get(), db.async.query(),
-- and worker.dispatch() — all yield the coroutine and let the event
-- loop serve other connections while waiting.

-- Allow outbound HTTP to self (localhost) for the fetch demos
app.manifest({
    hosts = {"127.0.0.1"},
    database = "async_demo.db",
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
    local resp = http.async.get("http://127.0.0.1:" .. port .. "/api/slow")
    res:json({ mode = "async", status = resp.status, body = resp.body })
end)

-- Async POST with body and headers
app.get("/async-post", function(req, res)
    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local resp = http.async.post("http://127.0.0.1:" .. port .. "/echo",
        '{"greeting":"hello"}',
        { headers = {["Content-Type"] = "application/json"} })
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

-- Async DB: offloads query to thread pool, event loop stays responsive
app.get("/async-db", function(_req, res)
    local rows = db.async.query("SELECT 1 + 1 as result")
    res:json({ rows = rows })
end)

-- Worker dispatch: runs Lua function on a worker thread with its own VM
app.get("/worker-dispatch", function(_req, res)
    local result = worker.dispatch(function(ctx)
        db.exec("CREATE TABLE IF NOT EXISTS async_test (id INTEGER PRIMARY KEY, val TEXT)")
        db.exec("INSERT INTO async_test (val) VALUES (?)", {ctx.value})
        local rows = db.query("SELECT COUNT(*) as n FROM async_test")
        return { count = rows[1].n }
    end, { value = "hello" })
    res:json(result)
end)

log.info("Async HTTP example loaded — routes registered")
