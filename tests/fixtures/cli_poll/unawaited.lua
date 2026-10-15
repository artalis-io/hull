local worker = require("hull.worker")
local db = require("hull.db").default()
app.manifest({ modules = { "hull/worker@1", "hull/db@1" } })
-- Audit 14 G1: main returns with work nobody joined - finished, running and
-- still queued. poll_pool_free delivers or cancels each one, so no
-- continuation outlives the runtime.
app.main(function(ctx)
    for i = 1, 16 do
        hull.async(function()
            worker.dispatch(function()
                local x = 0
                for j = 1, 3000000 do x = x + j end
                return x
            end)
        end)
        hull.async(function() db.async.query("SELECT ? AS value", { i }) end)
    end
    hull.async(function() hull.sleep(60000) end)
    hull.sleep(20)
    ctx.stdout:write("poll-cli: left work behind\n")
    return 0
end)
