-- Compute example — WASM compute plugins in Hull
--
-- Demonstrates sync compute.call() and async compute.async.call()
-- for offloading CPU-intensive work to sandboxed WASM modules.
--
-- Run: hull examples/compute/app.lua
-- Test: hull test examples/compute

-- Sync: blocks the request handler (good for fast/small calls)
app.get("/score", function(req, res)
    local input = req.query.text or "hello"
    local output, err = compute.call("score", input, {
        gas = 1000000,
    })
    if err then
        res:status(500):json({ error = err })
        return
    end
    -- Score module returns a single byte (0-100)
    local score = string.byte(output, 1) or 0
    res:json({
        text = input,
        score = score,
    })
end)

-- Sync echo
app.get("/echo", function(req, res)
    local input = req.query.text or ""
    local output, err = compute.call("echo", input)
    if err then
        res:status(500):json({ error = err })
        return
    end
    res:json({
        input = input,
        output = output,
        match = (input == output),
    })
end)

-- Async: yields to event loop, other requests served while WASM runs.
-- Use for expensive computations that would block the event loop.
-- Returns {result=...} or {error=...} (single table, not two values).
app.get("/async-echo", function(req, res)
    local input = req.query.text or ""
    local r = compute.async.call("echo", input)
    if r.error then
        res:status(500):json({ error = r.error })
        return
    end
    res:json({
        input = input,
        output = r.result,
        match = (input == r.result),
    })
end)

app.get("/health", function(req, res)
    res:json({ ok = true })
end)
