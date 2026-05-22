-- TUI picker — Hull + Lua TUI example
--
-- Run:  hull run app.lua
--
-- Demonstrates the canonical `tui.run` immediate-mode loop on top of
-- the `hull.tui` module: presents a scrollable list, returns the
-- picked entry on stdout, or exits with code 130 on Ctrl+C / Esc.
--
-- Keys:
--   ↑/↓ or j/k    move cursor
--   PgUp/PgDn     jump 10
--   Home/End      jump to top/bottom
--   Enter         pick
--   q / Esc       abort

local tui = require("hull.tui")

app.manifest({
    tui = true,
    modules = {
        "hull/tui@1",
    },
})

local fruits = {
    "apple",
    "apricot",
    "banana",
    "blueberry",
    "cantaloupe",
    "cherry",
    "date",
    "elderberry",
    "fig",
    "grape",
    "honeydew",
    "kiwi",
    "lemon",
    "mango",
    "nectarine",
    "orange",
    "papaya",
    "pear",
    "raspberry",
    "strawberry",
    "tangerine",
    "watermelon",
}

app.main(function(ctx)
    local picked = tui.list(fruits, {
        title = "Pick a fruit (↑/↓, enter, q to quit):",
    })

    if not picked then
        ctx.stderr:write("aborted\n")
        return 130
    end

    ctx.stdout:write(fruits[picked] .. "\n")
    return 0
end)
