-- stdlib/lua/hull/dev_tui.lua - `hull dev --tui` request log + status.
--
-- Architecture
--   The parent process (commands/dev.c) owns the child + the log
--   ring buffer. We're the renderer: on every tick (100ms) we
--   drain the pipe via tool.dev_drain, render the most-recent
--   lines, check for file changes, and react to key presses.
--
-- Keys
--   r              trigger reload (kill+respawn the child)
--   f              toggle the filter prompt (filter log lines)
--   c              clear the filter
--   q / Esc        quit
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local tui = require("hull.tui")

local function colors_for_theme(theme)
    if theme == "light" then
        return { ok = 0x006400, err = 0x8B0000, dim = 0x666666, head = 0x000080 }
    end
    return { ok = 0x00C000, err = 0xE06C75, dim = 0x888888, head = 0x61AFEF }
end

-- ── State ─────────────────────────────────────────────────────────

local state = {
    filter   = nil,                   -- nil or substring to grep
    show_filter_prompt = false,
    filter_buf = "",
    filter_cursor = 1,
    started_at_ms = time.now_ms(),  -- rough baseline for "uptime"
    palette = colors_for_theme(tui.theme()),
}

-- ── Helpers ───────────────────────────────────────────────────────

local function format_status_line(status, cols)
    if not status then return "(no dev state)" end
    local pid = status.alive
        and tostring(status.pid)
        or string.format("%d (DEAD)", status.pid or 0)
    local left = string.format(
        "pid=%s   reloads=%d   lines=%d   app=%s",
        pid, status.reload_count, status.log_count, status.app_dir)
    if #left > cols - 2 then left = left:sub(1, cols - 2) end
    return left
end

-- Pull recent lines that match the active filter (newest-first).
local function lines_for_display(max)
    local raw = tool.dev_recent_lines(max * 4)  -- overscan; filter trims
    if not state.filter or state.filter == "" then return raw end
    local out, n = {}, 0
    for _, line in ipairs(raw) do
        if line:find(state.filter, 1, true) then
            n = n + 1
            out[n] = line
            if n >= max then break end
        end
    end
    return out
end

-- Render the log area (the "request log" pane). Newest at the
-- bottom, like a tail.
local function render_log(t, lines, area_y, area_h, area_w)
    -- Reverse so we draw oldest→newest top→bottom.
    local h = math.min(area_h, #lines)
    for i = 0, h - 1 do
        local line = lines[h - i]
        if #line > area_w then line = line:sub(1, area_w) end
        t:print(1, area_y + i, line)
    end
end

local function render(t)
    t:clear()

    -- Title bar.
    t:style({ bold = true, fg = state.palette.head })
    t:print(1, 1, "hull dev   " ..
                  (state.status and state.status.app_dir or "?"))
    t:style({})

    -- Top status line (under title).
    t:print(1, 2, format_status_line(state.status, t.cols))

    -- Separator.
    t:style({ fg = state.palette.dim })
    t:print(1, 3, string.rep("─", t.cols))
    t:style({})

    -- Log area: rows 4 .. (rows - 2). Bottom row is the key bar /
    -- filter input.
    local log_y0 = 4
    local log_h  = t.rows - log_y0 - 1
    if log_h < 1 then log_h = 1 end
    local lines = lines_for_display(log_h)
    if #lines == 0 then
        t:style({ fg = state.palette.dim, italic = true })
        local empty = state.filter and ("(no lines match \"" .. state.filter .. "\")")
                                    or "(waiting for child output…)"
        t:print(1, log_y0, empty)
        t:style({})
    else
        render_log(t, lines, log_y0, log_h, t.cols)
    end

    -- Bottom row: either the filter input prompt OR the key bar.
    if state.show_filter_prompt then
        t:style({ fg = state.palette.head, bold = true })
        t:print(1, t.rows, "filter: ")
        t:style({})
        t:print(9, t.rows, state.filter_buf)
        t:move(9 + state.filter_cursor - 1, t.rows)
    else
        t:style({ fg = state.palette.dim })
        local bar = "r:reload  f:filter  c:clear-filter  q:quit"
        if state.filter then
            bar = bar .. "    filter=\"" .. state.filter .. "\""
        end
        t:print(1, t.rows, bar)
        t:style({})
    end
end

-- ── Event handling ────────────────────────────────────────────────

local function on_event(ev)
    if ev.kind ~= "key" then return end
    local k = ev.key

    if state.show_filter_prompt then
        -- Line-edit mode for the filter input.
        if k == "enter" then
            state.filter = state.filter_buf ~= "" and state.filter_buf or nil
            state.show_filter_prompt = false
        elseif k == "escape" then
            state.show_filter_prompt = false
        elseif k == "backspace" then
            if state.filter_cursor > 1 then
                state.filter_buf = state.filter_buf:sub(1, state.filter_cursor - 2)
                                   .. state.filter_buf:sub(state.filter_cursor)
                state.filter_cursor = state.filter_cursor - 1
            end
        elseif ev.codepoint and ev.codepoint >= 0x20 and not ev.ctrl and not ev.alt then
            state.filter_buf = state.filter_buf:sub(1, state.filter_cursor - 1)
                              .. ev.key
                              .. state.filter_buf:sub(state.filter_cursor)
            state.filter_cursor = state.filter_cursor + #ev.key
        end
        return
    end

    if k == "q" or k == "escape" or k == "ctrl+c" then
        return "quit"
    elseif k == "r" then
        tool.dev_reload()
    elseif k == "f" then
        state.show_filter_prompt = true
        state.filter_buf = state.filter or ""
        state.filter_cursor = #state.filter_buf + 1
    elseif k == "c" then
        state.filter = nil
    end
end

-- ── Tick ──────────────────────────────────────────────────────────
--
-- Called on every tui.run cycle. Drains stdin from the child into the
-- ring buffer, refreshes status, checks the file watcher, triggers
-- reload if needed.

local function tick()
    tool.dev_drain()
    state.status = tool.dev_status()
    -- File-watch reload: same heuristic as the non-tui dev path.
    if tool.dev_check_file_change() then
        tool.dev_reload()
    end
end

-- ── Main ──────────────────────────────────────────────────────────

state.status = tool.dev_status()

tui.run({
    draw = function(t)
        tick()
        render(t)
    end,
    on_event = on_event,
    tick_ms = 100,         -- 10Hz refresh
})

tool.exit(0)
