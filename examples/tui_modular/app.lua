-- Modular TUI scaffold.
--
-- One state table flows through every view. The active view is named
-- by state.view; views/<name>.lua exports a render() and a
-- handle_event() function. New view? Add views/foo.lua and let some
-- existing view set state.view = "foo".

app.manifest({
    tui = true,
    modules = {
        "hull/tui@1",
    },
})

local tui    = require("hull.tui")
local state0 = require("./lib/state")

local function load_view(name)
    -- pcall'd so a typo doesn't crash mid-render - fall back to the
    -- menu so the user has somewhere to go.
    local ok, mod = pcall(require, "./views/" .. name)
    if not ok then return require("./views/menu") end
    return mod
end

app.main(function(_ctx)
    local state = state0.initial()

    local exit_token = tui.run({
        draw = function(ctx)
            local view = load_view(state.view)
            view.render(ctx, state)
        end,
        on_event = function(ev)
            local view = load_view(state.view)
            local next_state, exit = view.handle_event(state, ev)
            if next_state then state = next_state end
            return exit  -- non-nil ends the loop
        end,
        tick_ms = 100,
    })

    return (exit_token == "quit") and 0 or 0
end)
