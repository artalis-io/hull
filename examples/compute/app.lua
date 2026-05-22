-- Compute example — WASM compute plugins in Hull
--
-- Demonstrates sync compute.call() and async compute.async.call()
-- for offloading CPU-intensive work to sandboxed WASM modules.
--
-- Run: hull examples/compute/app.lua
-- Test: hull test examples/compute

local compute = require("hull.compute")

app.manifest({
    compute = true,
    modules = {
    "hull/http-server@1",
        "hull/compute@1",
    },
})

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
-- Returns the output directly (string in buffer-less mode); throws on error.
app.get("/async-echo", function(req, res)
    local input = req.query.text or ""
    local ok, output = pcall(compute.async.call, "echo", input)
    if not ok then
        res:status(500):json({ error = tostring(output) })
        return
    end
    res:json({
        input = input,
        output = output,
        match = (input == output),
    })
end)

-- Buffer mode: zero-copy output (no string materialization until needed)
-- Use { buffer = true } to get an HlWasmBuffer instead of a string.
-- buf:bytes() materializes to a Lua string, buf:len() returns size, buf:close() frees.
app.get("/buffer-echo", function(req, res)
    local input = req.query.text or "hello"
    local buf, err = compute.call("echo", input, { buffer = true })
    if err then
        res:status(500):json({ error = err })
        return
    end
    res:json({
        input = input,
        output = buf:bytes(),  -- materializes here
        len = buf:len(),
    })
    buf:close()  -- explicit release (GC would also handle it)
end)

-- Buffer chaining: pipe output of one call into the next without copies.
-- Each intermediate result stays in WASM linear memory until consumed.
-- Only the final result is materialized to a Lua string.
app.get("/chain", function(req, res)
    local input = req.query.text or "chain test"

    -- Step 1: echo → buffer (no string copy)
    local buf1, err = compute.call("echo", input, { buffer = true })
    if err then res:status(500):json({ error = err }); return end

    -- Step 2: feed buffer directly to next call (reads from WASM memory)
    local buf2, err2 = compute.call("echo", buf1, { buffer = true })
    buf1:close()  -- release step 1 instance back to pool
    if err2 then res:status(500):json({ error = err2 }); return end

    -- Step 3: one more pass
    local buf3, err3 = compute.call("echo", buf2, { buffer = true })
    buf2:close()
    if err3 then res:status(500):json({ error = err3 }); return end

    -- Materialize only the final result
    local result = buf3:bytes()
    buf3:close()

    res:json({
        input = input,
        output = result,
        steps = 3,
        match = (input == result),
    })
end)

-- compute.buffer(): create a buffer from a string, useful for passing
-- pre-built data into compute calls without extra copies.
app.get("/from-buffer", function(req, res)
    local input = req.query.text or "buffer input"
    local buf = compute.buffer(input)
    local output, err = compute.call("echo", buf)
    buf:close()
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

-- Persistent instance: load once, call many times with preserved linear memory.
-- No pool overhead, no re-instantiation. Great for stateful workloads.
local echo_inst = compute.instance("echo")

app.get("/persistent", function(req, res)
    local input = req.query.text or "persistent"
    local output, err = echo_inst:call(input)
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

-- Persistent async: yields to event loop while WASM runs on thread pool.
app.get("/persistent-async", function(req, res)
    local input = req.query.text or "async persistent"
    local r = echo_inst:async_call(input)
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

-- Persistent instance info
app.get("/persistent-info", function(req, res)
    res:json({
        name = echo_inst:name(),
        closed = echo_inst:closed(),
    })
end)

app.get("/health", function(req, res)
    res:json({ ok = true })
end)
