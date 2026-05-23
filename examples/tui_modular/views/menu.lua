-- views/menu.lua — Top-level menu.
--
-- Convention: each view exports render(ctx, state) and
-- handle_event(state, ev). render is a function of state (no side
-- effects beyond ctx draw calls). handle_event returns
-- (next_state | nil, exit_token | nil); a non-nil exit_token ends
-- the event loop.

local M = {}

local items = {
    { label = "Show greeting",     view = "detail", message = "hello, hull" },
    { label = "Show current time", view = "detail", message = "(set by handle_event)" },
    { label = "Quit",              quit = true },
}

function M.render(ctx, state)
    ctx.move(1, 1); ctx.write("=== Menu ===")
    for i, item in ipairs(items) do
        ctx.move(i + 2, 3)
        if i == state.cursor then
            ctx.write("> " .. item.label)
        else
            ctx.write("  " .. item.label)
        end
    end
    ctx.move(#items + 4, 1)
    ctx.write("[ up/down / enter / q ]")
end

function M.handle_event(state, ev)
    if ev.kind ~= "key" then return nil, nil end
    local k = ev.key
    if k == "up" or k == "k" then
        state.cursor = math.max(1, state.cursor - 1)
        return state, nil
    elseif k == "down" or k == "j" then
        state.cursor = math.min(#items, state.cursor + 1)
        return state, nil
    elseif k == "enter" then
        local item = items[state.cursor]
        if item.quit then return state, "quit" end
        state.view    = item.view
        state.message = item.message
        return state, nil
    elseif k == "q" or k == "esc" then
        return state, "quit"
    end
    return nil, nil
end

return M
