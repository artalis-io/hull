-- lib/fmt.lua — Shared output-formatting helpers.
--
-- Anything reused across commands lives here. Keeping it in `lib/`
-- (not `commands/`) means it can be required from anywhere without
-- looking like a subcommand to the app.lua dispatcher.

local M = {}

function M.greeting(name)
    return "hello " .. name
end

return M
