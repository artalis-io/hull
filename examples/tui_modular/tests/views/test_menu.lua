-- TUI views are pure functions of state, so they're cheap to unit-test
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
