-- WebSocket server close fixture (tests/e2e_ws_close.sh, audit 44 M5): the
-- app closes the connection itself - from on_open on /open, from on_message
-- on /msg - and its on_close must still run once Keel delivers the close,
-- with the node removed from the registry (ws.connections back to 0).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
local ws = require("hull.web.ws-server")

app.manifest({ modules = { "hull/http-server@1", "hull/web/ws-server@1" } })

local closes = 0

app.get("/ping", function(_req, res) res:text("pong") end)

app.ws("/open", {
    on_open = function(conn) conn:close(1000, "bye") end,
    on_close = function(_conn) closes = closes + 1 end,
})

app.ws("/msg", {
    on_message = function(conn, _msg) conn:close(4000, "done") end,
    on_close = function(_conn) closes = closes + 1 end,
})

app.get("/state", function(_req, res)
    local sent = ws.broadcast("/open", "x") + ws.broadcast("/msg", "x")
    res:text("closes=" .. closes
        .. " open=" .. ws.connections("/open")
        .. " msg=" .. ws.connections("/msg")
        .. " sent=" .. sent)
end)
