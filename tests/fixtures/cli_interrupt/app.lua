-- app.main observes SIGINT / SIGTERM (shared/main_signal.h, platform gap 7).
-- Modes (ctx.args[1]):
--   handler  register a callback and poll ctx.interrupted(); return 3 once
--            the signal arrived
--   none     register nothing: the process ends 128 + signal, buffered
--            stdout flushed
--   stuck    register a callback but never finish: the grace ends main
app.manifest({ name = "cli-interrupt" })
app.main(function(ctx)
    local mode = ctx.args[1] or "handler"
    if mode == "none" then
        ctx.stdout:write("interrupt: ready\n")
        ctx.stdout:flush()
        ctx.stdout:write("interrupt: buffered\n")   -- flushed only by a clean exit
        hull.sleep(60000)
        return 0
    end
    ctx.on_interrupt(function(name)
        ctx.stdout:write("interrupt: handler " .. name .. "\n")
        ctx.stdout:flush()
    end)
    ctx.stdout:write("interrupt: ready\n")
    ctx.stdout:flush()
    if mode == "stuck" then
        hull.sleep(60000)
        return 0
    end
    while not ctx.interrupted() do
        hull.sleep(50)
    end
    ctx.stdout:write("interrupt: main saw " .. ctx.interrupted() .. "\n")
    return 3
end)
