local worker = require("hull.worker")
local db = require("hull.db").default()
app.manifest({ modules = { "hull/worker@1", "hull/db@1" } })
app.main(function(ctx)
    local total = 0
    for i = 1, 32 do
        hull.sleep(1)
        local result = worker.dispatch(function(input) return input.value + 1 end,
                                       { value = i })
        assert(result == i + 1, "worker result mismatch")
        local rows = db.async.query("SELECT ? AS value", { result })
        assert(rows[1].value == result, "async DB result mismatch")
        total = total + result
    end
    assert(total == 560)
    ctx.stdout:write("poll-cli: timers=32 workers=32 db=32 total=560\n")
    return ctx.args[1] == "exit7" and 7 or 0
end)
