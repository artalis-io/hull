-- stdlib/lua/hull/agent_context_tui.lua - interactive context picker.
--
-- Invoked by `hull agent context --interactive` (or `--tui`). Lists
-- every task the agent knows about; for each selected task, renders
-- the rendered context preview at the chosen detail level.
--
-- Two-pane layout: list of tasks on the left, content preview on
-- the right. ↑/↓ to navigate, ←/→ to switch levels, q to quit.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local tui = require("hull.tui")
local json = require("hull.json")

-- Same task set the C dispatcher's usage string lists (see
-- commands/agent.c::cmd_context). Keep in sync if a new agent task
-- is added.
local TASKS = {
    "auth", "db", "middleware", "templates", "routing", "testing",
    "build", "deploy", "search", "i18n", "webhooks", "validation",
    "compute",
}

local LEVELS = { "minimal", "compact", "full" }

-- Pre-fetch each (task, level) on first hit; cache so navigation
-- doesn't reload the same data twice.
local cache = {}
local function fetch(task, level)
    local key = task .. ":" .. level
    if cache[key] then return cache[key] end
    local raw, rc = tool.agent_context(task, level)
    if not raw then
        cache[key] = { error = "agent_context returned nothing (rc=" .. tostring(rc) .. ")" }
        return cache[key]
    end
    local ok, data = pcall(json.decode, raw)
    if not ok then
        cache[key] = { error = "json.decode failed: " .. tostring(data) }
        return cache[key]
    end
    cache[key] = data
    return data
end

-- Wrap a long string onto lines that fit `w` columns. Returns a
-- table of lines. Splits on whitespace where possible; falls back
-- to mid-word truncation for very long runs.
local function wrap_lines(text, w)
    if not text or text == "" then return { "" } end
    local out = {}
    for paragraph in text:gmatch("[^\n]+") do
        local line = ""
        for word in paragraph:gmatch("%S+") do
            if #line == 0 then
                line = word
            elseif #line + 1 + #word <= w then
                line = line .. " " .. word
            else
                out[#out + 1] = line
                if #word > w then
                    out[#out + 1] = word:sub(1, w)
                    line = ""
                else
                    line = word
                end
            end
        end
        if #line > 0 then out[#out + 1] = line end
        if paragraph == "" then out[#out + 1] = "" end
    end
    return out
end

local function render(t, state)
    t:clear()

    -- Title bar.
    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, 1, "hull agent context   task=" .. state.task ..
                  "   level=" .. state.level)
    t:style({})

    -- Left pane: task list (fixed 18 cols).
    local left_w = 18
    for i, task in ipairs(TASKS) do
        local y = 3 + i - 1
        if y > t.rows - 2 then break end
        if i == state.cursor then
            t:style({ reverse = true })
            t:print(1, y, "> " .. task .. string.rep(" ", left_w - #task - 2))
            t:style({})
        else
            t:print(1, y, "  " .. task)
        end
    end

    -- Right pane: content preview.
    local right_x = left_w + 2
    local right_w = math.max(20, t.cols - right_x)
    local data = state.data
    if data.error then
        t:style({ fg = 0xE06C75, bold = true })
        t:print(right_x, 3, "Error: " .. data.error)
        t:style({})
    elseif type(data.content) == "string" and data.content ~= "" then
        local lines = wrap_lines(data.content, right_w - 1)
        local h = t.rows - 4
        for i = 1, math.min(#lines, h) do
            t:print(right_x, 2 + i, lines[i])
        end
        if #lines > h then
            t:style({ fg = 0x888888, italic = true })
            t:print(right_x, t.rows - 1,
                    "(" .. (#lines - h) .. " more line"
                    .. ((#lines - h) == 1 and "" or "s") .. " - open with"
                    .. " hull agent context --task=" .. state.task ..
                       " --level=full)")
            t:style({})
        end
    else
        t:style({ fg = 0x888888, italic = true })
        t:print(right_x, 3, "(no content for " .. state.task ..
                            " at level " .. state.level .. ")")
        t:style({})
    end

    -- Vertical separator.
    t:style({ fg = 0x444444 })
    for y = 2, t.rows - 1 do
        t:print(left_w + 1, y, "│")
    end
    t:style({})

    -- Key bar.
    t:style({ fg = 0x888888 })
    t:print(1, t.rows, "↑/↓:task   ←/→:level   enter:print to stdout   q:quit")
    t:style({})
end

-- ── Main ───────────────────────────────────────────────────────────

local function level_index(level)
    for i, l in ipairs(LEVELS) do if l == level then return i end end
    return 2  -- "compact"
end

local state = {
    cursor = 1,
    task   = TASKS[1],
    level  = "compact",
}
state.level_idx = level_index(state.level)
state.data = fetch(state.task, state.level)

local final_action = nil

tui.run({
    draw = function(t) render(t, state) end,
    on_event = function(ev)
        if ev.kind ~= "key" then return end
        local k = ev.key
        if k == "q" or k == "escape" or k == "ctrl+c" then
            return "quit"
        elseif k == "up" or k == "k" then
            state.cursor = math.max(1, state.cursor - 1)
            state.task = TASKS[state.cursor]
            state.data = fetch(state.task, state.level)
        elseif k == "down" or k == "j" then
            state.cursor = math.min(#TASKS, state.cursor + 1)
            state.task = TASKS[state.cursor]
            state.data = fetch(state.task, state.level)
        elseif k == "right" or k == "l" then
            state.level_idx = math.min(#LEVELS, state.level_idx + 1)
            state.level = LEVELS[state.level_idx]
            state.data = fetch(state.task, state.level)
        elseif k == "left" or k == "h" then
            state.level_idx = math.max(1, state.level_idx - 1)
            state.level = LEVELS[state.level_idx]
            state.data = fetch(state.task, state.level)
        elseif k == "enter" then
            final_action = { task = state.task, level = state.level }
            return "print"
        end
    end,
    tick_ms = -1,
})

-- On "enter", print the chosen context to stdout (same shape as
-- non-interactive `hull agent context` JSON output). This lets the
-- TUI integrate with shell pipelines: pick interactively, then
-- pipe the chosen context elsewhere.
if final_action then
    local raw = tool.agent_context(final_action.task, final_action.level)
    -- `print` is always available in tool-mode VMs; io.* is sandboxed.
    if raw then print(raw) end
end

tool.exit(0)
