-- routes/health.lua — liveness + readiness endpoints.
--
-- The health endpoint is intentionally trivial and unauthenticated:
-- a loadbalancer or container orchestrator hits it to decide whether
-- to route traffic to this process. Anything heavier (DB ping etc.)
-- belongs in /ready, which can use the stdlib `hull.middleware.health`
-- module when you're ready to wire it up.

local M = {}

function M.register(app)
    app.get("/health", function(_req, res)
        res:json({ status = "ok" })
    end)
end

return M
