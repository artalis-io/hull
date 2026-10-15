-- Shutdown fixture (tests/e2e_teardown.sh): at SIGTERM it holds a request
-- parked on hull.sleep, a WebSocket that never answers the close, and a
-- detached http.fetch started from a timer - each of which the teardown used
-- to reach through a freed runtime (audit 14 G1 / audit 15).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
local http_client = require("hull.http-client")
local ws_server = require("hull.web.ws-server")

app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1", "hull/timers@1",
                "hull/web/ws-server@1" },
    hosts = { "127.0.0.1" },
})

local target, fired = nil, false

-- A detached fetch: no request holds it.
app.every(100, function()
    if not target or fired then return end
    fired = true
    http_client.async.get(target)
end)

app.get("/ping", function(_req, res) res:text("pong") end)
app.get("/arm/:port", function(req, res)
    target = "http://127.0.0.1:" .. req.params.port .. "/slow"
    res:text("armed")
end)
app.get("/fired", function(_req, res) res:text(fired and "yes" or "no") end)
app.get("/slow", function(_req, res)
    hull.sleep(60000)
    res:text("late")
end)

app.ws("/ws", {
    on_open = function(conn) conn:send("hello") end,
    on_message = function(conn, msg) conn:send(msg) end,
    -- Runs when a client closes normally. A WebSocket still open when the
    -- server is torn down closes without it: the loop is gone by then.
    on_close = function(_conn) ws_server.broadcast("/ws", "left") end,
})
