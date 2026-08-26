-- commands/greet.lua - Print a greeting.
--
-- Convention: each command file exports a single `run(ctx)` function.
-- ctx is the same shape app.main receives: args / env / stdin / stdout
-- / stderr. The return value is the process exit code (nil → 0).

local fmt = require("./../lib/fmt")

local M = {}

function M.run(ctx)
    local name = ctx.args[1]
    if not name then
        ctx.stderr:write("greet: missing NAME argument\n")
        return 2
    end
    ctx.stdout:write(fmt.greeting(name) .. "\n")
    return 0
end

return M
