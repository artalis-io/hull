-- examples/workflows (Lua) - durable workflow-as-code + observability.
--
-- A workflow is a job whose body is ordinary code: `ctx.step(name, fn)` runs a
-- step ONCE and memoizes its result, so a crash or retry resumes past completed
-- steps instead of re-charging a card. `ctx.wait_signal` parks the workflow
-- durably until an external event arrives; saga `compensate` callbacks roll back
-- completed steps on terminal failure; `ctx.now/uuid` stay stable across replays.
--
-- This models order fulfillment: charge -> reserve stock -> WAIT for a shipping
-- confirmation (could be minutes or days) -> notify. The workflow instance IS a
-- job, so it survives restarts, retries with backoff, and reports progress.
--
-- Try it:
--   hull examples/workflows/app.lua -d ./wf.db
--   ID=$(curl -s -X POST localhost:3000/orders -d '{"sku":"A1","card":"tok_ok"}' | tr -dc 0-9)
--   curl localhost:3000/orders/$ID                 # {status:"waiting", steps_done:["charge","reserve"]}
--   curl -X POST localhost:3000/orders/$ID/ship -d '{"tracking":"1Z999"}'
--   curl localhost:3000/orders/$ID                 # {status:"done", result:{...}}
--   curl localhost:3000/metrics                    # gauges + latency percentiles
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local jobs = require("hull.jobs")
local json = require("hull.json")
local log  = require("hull.log")

app.manifest({
    modules = {
        "hull/jobs@1",
        "hull/json@1",
        "hull/http-server@1",
        "hull/timers@1",
        "hull/log@1",
    },
})

-- history = true turns on the opt-in attempt-history table, which powers the
-- latency / throughput fields of jobs.metrics (gauges work without it).
jobs.init({ history = true })

-- A tiny in-memory "inventory" so the saga compensation is observable. Real code
-- would use db.* here; the point is the ROLLBACK, not the store.
local reserved = {}

-- ── The workflow ─────────────────────────────────────────────────────────────
-- Register once at startup. `ctx.input` is the payload from jobs.start.
jobs.workflow("fulfill_order", function(ctx)
    local order = ctx.input

    -- Step 1: charge the card. Memoized: if the workflow resumes after this
    -- point, the charge is NOT repeated - ctx.step returns the recorded result.
    -- A declined card returns ok=false (no charge made); the body then dead-
    -- letters below. ctx.uuid()/ctx.now() are memoized (byte-identical on every
    -- replay), so the receipt id is stable when the body re-runs.
    local charge = ctx.step("charge", function()
        if order.card == "tok_declined" then return { ok = false } end
        return { ok = true, receipt = ctx.uuid(), amount = order.amount or 4200, at = ctx.now() }
    end)
    if not charge.ok then
        return jobs.DEAD   -- non-retryable; nothing reserved yet, nothing to roll back
    end

    -- Step 2: reserve inventory, WITH a compensation. If the workflow fails
    -- terminally after this, `compensate` runs (reverse order) to release stock.
    ctx.step("reserve", function()
        reserved[ctx.id] = order.sku or "A1"
        log.info("reserved stock for order " .. ctx.id)
        return true
    end, {
        compensate = function()
            reserved[ctx.id] = nil
            log.warn("compensated: released stock for order " .. ctx.id)
        end,
    })

    -- A fraud flag fails the order AFTER reserving: returning jobs.DEAD from the
    -- body dead-letters it and runs the saga compensations (releasing the stock).
    if order.card == "tok_fraud" then
        return jobs.DEAD
    end

    -- Step 3: park until the warehouse confirms shipment. The workflow yields to
    -- the durable 'waiting' status here - no worker CPU is held while it waits.
    -- jobs.signal(id, "shipped", payload) wakes it; the payload is the return.
    local ship = ctx.wait_signal("shipped")

    -- Step 4: notify the customer (memoized, so a resume won't double-send).
    ctx.step("notify", function()
        log.info("order " .. ctx.id .. " shipped, tracking " .. tostring(ship and ship.tracking))
        return true
    end)

    -- The return value becomes the workflow result (jobs.result / jobs.await).
    return {
        order_id = ctx.id,
        receipt  = charge.receipt,
        amount   = charge.amount,
        tracking = ship and ship.tracking,
    }
end)

-- In-process execution model: an app.every timer drives jobs.work, which claims
-- and advances workflows (and fires the reaper). For a dedicated worker process
-- instead, see worker.lua.
app.every(500, function() jobs.work({ batch = 10 }) end)

-- ── Routes ───────────────────────────────────────────────────────────────────

-- Start an order-fulfillment workflow. Returns the workflow (job) id.
app.post("/orders", function(req, res)
    local body = json.decode(req.body or "{}") or {}
    local id = jobs.start("fulfill_order", body)
    res:json({ order = id })
end)

-- Inspect a workflow instance: status + which steps have run + the result.
app.get("/orders/:id", function(req, res)
    local st = jobs.workflow_status(tonumber(req.params.id))
    if not st then res:status(404):json({ error = "no such order" }); return end
    res:json(st)
end)

-- Deliver the "shipped" signal, waking a workflow parked on wait_signal.
app.post("/orders/:id/ship", function(req, res)
    local body = json.decode(req.body or "{}") or {}
    jobs.signal(tonumber(req.params.id), "shipped", { tracking = body.tracking })
    res:json({ signalled = true })
end)

-- Observability: DB-derived fleet-correct gauges + (with history on) latency
-- percentiles and throughput. Point a dashboard at this.
app.get("/metrics", function(req, res)
    res:json(jobs.metrics())
end)

-- Live saga state: which orders currently hold reserved stock. A successful
-- order keeps its entry; a compensated (fraud-dead-lettered) order has its
-- entry released - so the rollback is observable right here.
app.get("/inventory", function(req, res)
    local held = {}
    for id, sku in pairs(reserved) do held[tostring(id)] = sku end
    res:json({ reserved = held })
end)
