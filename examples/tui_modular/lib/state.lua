-- lib/state.lua - Initial state factory and shared helpers.
--
-- State is a single table threaded through every render/event call.
-- Keep it serialisable (no closures or userdata) so it's easy to
-- snapshot / log / replay during debugging.

local M = {}

function M.initial()
    return {
        view = "menu",       -- name of the current view in views/
        cursor = 1,          -- selected row in menu
        message = nil,       -- optional message shown in detail view
    }
end

return M
