-- Compute example — WASM compute plugins in Hull
--
-- Demonstrates using compute.call() to offload CPU-intensive
-- work to a sandboxed WASM module.
--
-- Run: hull examples/compute/app.lua
-- Test: hull test examples/compute

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

app.get("/health", function(req, res)
    res:json({ ok = true })
end)
