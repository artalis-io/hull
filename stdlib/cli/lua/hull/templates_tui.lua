--
-- hull.templates_tui — `hull new --type tui <name>` scaffolding
--
-- Modular TUI layout. app.lua holds the immediate-mode event loop
-- (via `tui.run({ draw, on_event })`) and a single `state` table.
-- Views in views/<name>.lua export `render(ctx, state)` (draws frame)
-- and `handle_event(state, ev)` (returns mutation or exit token).
-- Routing between views is just `state.view = "X"`.
--
-- Lua-only — Hull's `hull.tui` module is Lua-only today. If a JS TUI
-- ever lands, drop in a parallel js_files table here and remove the
-- runtime check in M.files().
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

local lua_files = {}

lua_files["app.lua"] = [[-- Modular TUI scaffold.
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
    -- pcall'd so a typo doesn't crash mid-render — fall back to the
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
]]

lua_files["lib/state.lua"] = [[-- lib/state.lua - Initial state factory and shared helpers.
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
]]

lua_files["views/menu.lua"] = [[-- views/menu.lua - Top-level menu.
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
]]

lua_files["views/detail.lua"] = [[-- views/detail.lua - Detail view shown when a menu item is picked.
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
]]

lua_files["tests/views/test_menu.lua"] = [[-- TUI views are pure functions of state, so they're cheap to unit-test
-- without a terminal. Just construct a state, feed an event, assert
-- the new state.

local menu = require("./../../views/menu")

test("menu: down arrow moves cursor", function()
    local s = { view = "menu", cursor = 1 }
    local next_s, exit = menu.handle_event(s, { kind = "key", key = "down" })
    test.eq(next_s.cursor, 2)
    test.eq(exit, nil)
end)

test("menu: q quits", function()
    local s = { view = "menu", cursor = 1 }
    local _, exit = menu.handle_event(s, { kind = "key", key = "q" })
    test.eq(exit, "quit")
end)
]]

-- ── Shared template (runtime-agnostic) ───────────────────────────────

local gitignore = [[*.key
hull.sig
build/
]]

-- ── Public API ───────────────────────────────────────────────────────

function M.files(runtime)
    -- TUI is Lua-only today; reject JS request explicitly so the
    -- scaffolder fails fast with a useful message instead of
    -- producing an empty directory.
    if runtime == "js" then
        return nil, "hull/tui@1 is Lua-only; rerun without --runtime js"
    end
    local copy = {}
    for k, v in pairs(lua_files) do copy[k] = v end
    copy[".gitignore"] = gitignore
    return copy, nil
end

return M
