-- Chat — WebSocket + SSE example
--
-- Run: hull app.lua -p 3000
--
-- Endpoints:
--   WS  /ws/chat          — WebSocket chat (broadcast messages)
--   SSE /sse/events       — SSE event stream (sends tick every 2s)
--   GET /health           — health check
--   GET /ws/connections   — current WebSocket connection count

local time = require("hull.time")
local ws   = require("hull.ws")

app.manifest({
    modules = {
        "hull/time@1",
        "hull/ws@1",
    },
})

-- ── WebSocket chat ────────────────────────────────────────────────

app.ws("/ws/chat", {
    on_open = function(conn)
        log.info("ws: client connected id=" .. conn:id())
        ws.broadcast("/ws/chat", json.encode({
            type = "join",
            id = conn:id(),
            connections = ws.connections("/ws/chat"),
        }))
    end,

    on_message = function(conn, msg)
        -- Broadcast message to all connected clients
        ws.broadcast("/ws/chat", json.encode({
            type = "message",
            from = conn:id(),
            data = msg,
        }))
    end,

    on_close = function(conn, code, reason)
        log.info("ws: client disconnected id=" .. conn:id()
                 .. " code=" .. tostring(code))
        ws.broadcast("/ws/chat", json.encode({
            type = "leave",
            id = conn:id(),
            code = code,
            connections = ws.connections("/ws/chat"),
        }))
    end,
})

-- ── SSE event stream ──────────────────────────────────────────────

app.sse("/sse/events", function(req, stream)
    stream:comment("connected")
    stream:event("welcome", json.encode({ time = time.datetime() }))

    for i = 1, 3 do
        hull.sleep(2000)
        stream:event("tick", json.encode({
            seq = i,
            time = time.datetime(),
        }), tostring(i))
    end

    stream:event(nil, "stream complete")
    stream:close()
end)

-- ── HTTP endpoints ────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok", runtime = "lua" })
end)

app.get("/ws/connections", function(_req, res)
    res:json({ count = ws.connections("/ws/chat") })
end)

log.info("Chat app loaded — routes registered")
