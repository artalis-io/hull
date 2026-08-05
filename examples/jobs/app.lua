-- examples/jobs (Lua) - durable background jobs with the in-process poller.
--
-- Enqueue work from a request handler; process it out-of-band with retries,
-- backoff, and a dead-letter path. This app uses the SINGLE-PROCESS model: an
-- `app.every` timer drives `jobs.work` on the event-loop thread. For a
-- dedicated worker process instead, see worker.lua + the README.
--
-- Try it:
--   hull examples/jobs/app.lua -d ./jobs.db
--   curl -X POST localhost:3000/jobs -d '{"type":"send_email","data":{"to":"a@b.c"}}'
--   curl -X POST localhost:3000/jobs -d '{"type":"flaky"}'
--   curl localhost:3000/jobs/stats
--   curl localhost:3000/jobs/dead
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

-- Create _hull_jobs + indexes (idempotent - safe on every boot).
jobs.init()

-- A normal handler: return nothing -> the job is done.
jobs.handler("send_email", function(job)
    log.info("send_email -> " .. tostring(job.data and job.data.to))
    -- (real code would call the email cap here; raising retries with backoff)
end)

-- A flaky handler: fails the first two attempts, succeeds on the third. Raising
-- an error reschedules with exponential backoff until max_attempts, then the
-- job dead-letters. `job.attempts` is the count of the attempt now running.
jobs.handler("flaky", function(job)
    if (job.attempts or 0) < 3 then
        error("transient failure (attempt " .. tostring(job.attempts) .. ")")
    end
    log.info("flaky succeeded on attempt " .. tostring(job.attempts))
end)

-- Catch-all for unregistered types.
jobs.default(function(job)
    log.warn("no handler for job type '" .. tostring(job.type) .. "'")
    return jobs.DISCARD
end)

-- In-process execution model: drain a batch every 500ms on the event loop.
-- (A handler that does http.fetch / db.async yields, so the loop keeps serving.)
app.every(500, function() jobs.work({ batch = 20 }) end)

-- Nightly retention sweep so done/dead rows don't accumulate forever.
app.daily("03:00", function() jobs.cleanup({ older_than = 7 * 86400 }) end)

-- ── Routes ──────────────────────────────────────────────────────────────────

-- Enqueue a job. Body: { type, data?, opts? } where opts can carry
-- queue / priority / delay / run_at / max_attempts / dedup_key.
app.post("/jobs", function(req, res)
    local body = json.decode(req.body or "{}") or {}
    local id = jobs.enqueue(body.type or "send_email", body.data or {}, body.opts or {})
    res:json({ enqueued = id })   -- id is nil when a dedup_key collapsed it
end)

-- Status counts (pending / running / done / dead).
app.get("/jobs/stats", function(req, res)
    res:json(jobs.stats())
end)

-- Inspect the dead-letter queue.
app.get("/jobs/dead", function(req, res)
    res:json({ dead = jobs.dead({ limit = 50 }) })
end)

-- Requeue a dead job for another run (fresh attempt budget).
app.post("/jobs/retry/:id", function(req, res)
    res:json({ requeued = jobs.retry(tonumber(req.params.id)) })
end)
