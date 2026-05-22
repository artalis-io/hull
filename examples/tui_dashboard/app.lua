-- TUI dashboard — Hull + Lua TUI showcase.
--
-- Demonstrates the multi-pane layout primitives:
--   - tui.frame with single/double/round borders
--   - tui.spinner / tui.progress as in-pane elements
--   - tui.async for a background ticker that updates state
--   - opt-in mouse mode (click rows to toggle them)
--
-- Run:  hull run app.lua
-- Keys: arrow keys / j/k to move; space/enter/click toggles; q quits.

local tui = require("hull.tui")

app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})

local METRICS = {
    { name = "requests/sec",  value = 0,   max = 200 },
    { name = "p50 latency",   value = 4,   max = 50, unit = "ms" },
    { name = "p99 latency",   value = 23,  max = 200, unit = "ms" },
    { name = "open conns",    value = 12,  max = 100 },
    { name = "queue depth",   value = 0,   max = 100 },
}

local TASKS = {
    { name = "Apply schema migrations",      done = true  },
    { name = "Warm template cache",          done = true  },
    { name = "Prime DNS resolver",           done = false },
    { name = "Open WebSocket pool",          done = false },
    { name = "Subscribe to outbox feed",     done = false },
    { name = "Negotiate TLS with upstream",  done = false },
}

app.main(function(ctx)
    local state = {
        spinner    = 0,
        cursor     = 1,
        bg_ticks   = 0,
        running    = true,
        -- Captured during `draw` so `on_event` (a separate closure)
        -- can compute the same mid-column split for mouse hit-testing
        -- without hardcoding a column count.
        cols       = 0,
        rows       = 0,
    }

    -- Background ticker — updates metrics every 250ms while the
    -- main coroutine sits in tui.poll. Proves the dashboard isn't
    -- frozen by a blocking renderer.
    tui.async(function()
        while state.running do
            hull.sleep(250)
            state.bg_ticks = state.bg_ticks + 1
            for _, m in ipairs(METRICS) do
                -- Random walk inside [0, m.max].
                local delta = math.random(-3, 4)
                m.value = math.max(0, math.min(m.max, m.value + delta))
            end
        end
    end)

    local result = tui.run({
        draw = function(t)
            t:clear()

            local _, frame_state = tui.spinner(state.spinner)
            state.spinner = frame_state

            -- Title.
            t:style({ bold = true, fg = 0x61AFEF })
            t:print(2, 1, "Hull TUI dashboard   tick #" .. state.bg_ticks ..
                          "   " .. tui.spinner(state.spinner))
            t:style({})

            -- Layout: left = metrics, right top = tasks, right bottom = help.
            state.cols = t.cols
            state.rows = t.rows
            local mid    = math.floor(t.cols * 0.5)
            local left_w = mid - 2
            local right_x = mid + 1
            local right_w = t.cols - right_x - 1
            local tasks_h = math.max(3, math.floor(t.rows * 0.55) - 2)

            -- ── Metrics pane ──
            tui.frame({
                x = 1, y = 2, w = left_w, h = t.rows - 3,
                title = "metrics",
                border = "round",
                style = { fg = 0x888888 },
            }, function(inner)
                for i, m in ipairs(METRICS) do
                    local y = 1 + (i - 1) * 2
                    if y > inner.h - 1 then break end
                    inner:print(1, y, m.name)
                    local bar = tui.progress(m.value / m.max,
                                              { width = math.max(8, inner.w - 24) })
                    local detail = string.format("%s   %d%s",
                        bar, m.value, m.unit or "")
                    if #detail > inner.w - 1 then
                        detail = detail:sub(1, inner.w - 1)
                    end
                    inner:print(1, y + 1, detail)
                end
            end)

            -- ── Tasks pane ──
            tui.frame({
                x = right_x, y = 2, w = right_w, h = tasks_h,
                title = "startup tasks",
                border = "round",
                style = { fg = 0x888888 },
            }, function(inner)
                for i, task in ipairs(TASKS) do
                    if i > inner.h then break end
                    local glyph = task.done and "✓" or "·"
                    local prefix = (i == state.cursor) and "> " or "  "
                    local label  = prefix .. glyph .. " " .. task.name
                    if i == state.cursor then
                        inner:print(1, i, label)
                        t:style({ fg = task.done and 0x00C000 or 0xE5C07B,
                                  reverse = true })
                        t:print(right_x + 1, 2 + i, label .. string.rep(" ",
                            math.max(0, inner.w - #label)))
                        t:style({})
                    else
                        t:style({ fg = task.done and 0x00C000 or 0x888888 })
                        inner:print(1, i, label)
                        t:style({})
                    end
                end
            end)

            -- ── Help pane ──
            tui.frame({
                x = right_x, y = 2 + tasks_h, w = right_w,
                h = t.rows - tasks_h - 4,
                title = "help",
                border = "single",
                style = { fg = 0x888888 },
            }, function(inner)
                local lines = {
                    "↑/↓ or j/k : move cursor",
                    "space/enter: toggle task",
                    "click      : toggle task by row",
                    "q / esc    : quit",
                    "",
                    "Metrics update every 250ms",
                    "via a background tui.async",
                    "coroutine while the main",
                    "loop sits in tui.poll(-1).",
                }
                for i, line in ipairs(lines) do
                    if i > inner.h then break end
                    inner:print(1, i, line)
                end
            end)

            -- Bottom hint.
            t:style({ fg = 0x888888 })
            t:print(2, t.rows, "theme=" .. tui.theme() ..
                              "   cap.mouse=" ..
                              (tui.caps().mouse and "yes" or "no"))
            t:style({})
        end,
        on_event = function(ev)
            if ev.kind == "key" then
                local k = ev.key
                if k == "q" or k == "escape" or k == "ctrl+c" then
                    return "quit"
                elseif k == "up" or k == "k" then
                    state.cursor = math.max(1, state.cursor - 1)
                elseif k == "down" or k == "j" then
                    state.cursor = math.min(#TASKS, state.cursor + 1)
                elseif k == "space" or k == " " or k == "enter" then
                    TASKS[state.cursor].done = not TASKS[state.cursor].done
                end
            elseif ev.kind == "mouse" then
                -- Click in the tasks pane toggles that row. The
                -- pane starts at (mid, 2) with a 1-cell border, so
                -- the first task row is at y=3. `state.cols` is
                -- updated each draw so the split tracks resizes.
                if ev.button == 0 and state.cols > 0 then  -- left click
                    local mid = math.floor(state.cols * 0.5)
                    local row = ev.y - 2
                    if row >= 1 and row <= #TASKS and ev.x > mid then
                        state.cursor = row
                        TASKS[row].done = not TASKS[row].done
                    end
                end
            end
        end,
        tick_ms  = 200,
        mouse    = true,
    })

    state.running = false
    hull.sleep(300)
    ctx.stdout:write("dashboard: exited with token=" .. tostring(result) .. "\n")
    return 0
end)
