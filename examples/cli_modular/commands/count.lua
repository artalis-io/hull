-- commands/count.lua — Count items passed as arguments.

local M = {}

function M.run(ctx)
    ctx.stdout:write(tostring(#ctx.args) .. "\n")
    return 0
end

return M
