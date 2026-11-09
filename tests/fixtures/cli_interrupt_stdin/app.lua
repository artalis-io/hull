-- app.main reads stdin AFTER an interrupt was delivered: main's own code may
-- still call ctx.stdin (audit 44, the Lua twin of the JS case).
app.manifest({ name = "cli-interrupt-stdin" })
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
    local ok, line = pcall(function() return ctx.stdin:read("*l") end)
    ctx.stdout:write("interrupt: stdin " .. tostring(ok) .. " " .. tostring(line) .. "\n")
    ctx.stdout:flush()
    return 4
end)
