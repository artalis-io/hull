-- Shutdown fixture (tests/e2e_teardown.sh): a WebSocket on_close that is
-- still parked on hull.sleep when the server is torn down. Its continuation
-- is cancelled, and the cancel runs the deferred conn teardown the
-- completion would have run (the JS twin aborted without it, audit 19).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
local log = require("hull.log")

app.manifest({ modules = { "hull/http-server@1", "hull/web/ws-server@1", "hull/log@1" } })

app.get("/ping", function(_req, res) res:text("pong") end)

app.ws("/ws", {
    on_open = function(conn) conn:send("hello") end,
    on_close = function(_conn)
        log.info("teardown-ws: on_close parked")
        hull.sleep(60000)
        log.info("teardown-ws: on_close resumed")
    end,
})
