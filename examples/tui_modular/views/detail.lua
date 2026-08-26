-- views/detail.lua - Detail view shown when a menu item is picked.
--
-- Esc / q / Backspace returns to the menu (state.view = "menu").

local M = {}

function M.render(ctx, state)
    ctx.move(1, 1); ctx.write("=== Detail ===")
    ctx.move(3, 3); ctx.write(state.message or "(no message)")
    ctx.move(5, 1); ctx.write("[ esc / backspace to return ]")
end

function M.handle_event(state, ev)
    if ev.kind ~= "key" then return nil, nil end
    if ev.key == "esc" or ev.key == "q" or ev.key == "backspace" then
        state.view = "menu"
        return state, nil
    end
    return nil, nil
end

return M
