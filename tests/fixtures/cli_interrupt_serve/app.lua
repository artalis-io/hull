-- app.main AND a route (main-then-serve, serve.c runner). main handles the
-- interrupt and returns 0 within the grace: the stop request must still end
-- the process (audit 44 M6) - it must not fall through into the serve loop.
app.manifest({ name = "cli-interrupt-serve", modules = { "hull/http-server@1" } })
app.get("/ping", function(_req, res) res:text("pong") end)
app.main(function(ctx)
    ctx.on_interrupt(function(name)
        ctx.stdout:write("interrupt: handler " .. name .. "\n")
        ctx.stdout:flush()
    end)
    ctx.stdout:write("interrupt: ready\n")
    ctx.stdout:flush()
    while not ctx.interrupted() do
        hull.sleep(50)
    end
    ctx.stdout:write("interrupt: main saw " .. ctx.interrupted() .. "\n")
    ctx.stdout:flush()
    return 0
end)
