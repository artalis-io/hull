-- app.main that returns with work still in flight (tests/e2e_teardown.sh):
-- worker.dispatch and db.async tasks nobody joins, a pending hull.sleep, and -
-- given a port - a detached http.fetch that never answers (audit 14 G1).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
local worker = require("hull.worker")
local db = require("hull.db").default()
local http_client = require("hull.http-client")

app.manifest({
    modules = { "hull/worker@1", "hull/db@1", "hull/http-client@1" },
    hosts = { "127.0.0.1" },
})

app.main(function(ctx)
    local port = ctx.args[1]
    if port then
        hull.async(function()
            http_client.async.get("http://127.0.0.1:" .. port .. "/slow")
        end)
    end
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
    hull.sleep(50)
    ctx.stdout:write("teardown-main: left work behind\n")
    return 0
end)
