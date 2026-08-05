-- examples/jobs (Lua) - the DEDICATED WORKER model.
--
-- Same handlers as app.lua, but instead of an `app.every` poller inside a web
-- app, this is a standalone process whose app.main runs the blocking claim
-- loop. Run K copies for horizontal scale; each claims disjoint jobs via the
-- atomic claim (SKIP LOCKED on Postgres/MySQL, serialized on SQLite).
--
-- Run it against the SAME database the web app enqueues into:
--   hull examples/jobs/worker.lua -d ./jobs.db
--   # or, discoverable via the CLI (identical effect):
--   hull jobs worker examples/jobs/worker.lua -d ./jobs.db
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local jobs = require("hull.jobs")
local log  = require("hull.log")

app.manifest({ modules = { "hull/jobs@1", "hull/log@1" } })

app.main(function()
    jobs.init()

    jobs.handler("send_email", function(job)
        log.info("send_email -> " .. tostring(job.data and job.data.to))
    end)
    jobs.handler("flaky", function(job)
        if (job.attempts or 0) < 3 then
            error("transient failure (attempt " .. tostring(job.attempts) .. ")")
        end
    end)

    log.info("worker started - draining the 'default' queue")
    -- Blocks: claim a batch, dispatch, reap; sleep 500ms when idle. Runs until
    -- the process is signalled (SIGINT/SIGTERM); an in-flight job is then
    -- reclaimed by the visibility-timeout reaper (handlers are at-least-once).
    jobs.run_worker({ batch = 20, poll_ms = 500 })
    return 0
end)
