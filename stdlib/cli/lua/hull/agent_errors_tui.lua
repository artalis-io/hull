-- stdlib/lua/hull/agent_errors_tui.lua - last-reload error scroller.
--
-- Invoked by `hull agent errors --tui`. Renders the .hull/last_error.json
-- structured output as a scrollable list with expandable detail panes.
--
-- The agent_errors JSON shape varies a bit by error type (load
-- failure vs. manifest error vs. runtime panic), so we display a
-- few well-known keys and dump the rest as a JSON blob in the
-- detail pane.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local tui = require("hull.tui")
local json = require("hull.json")

local function fetch(app_dir)
    local raw, rc = tool.agent_errors(app_dir or ".")
    if not raw then
        return nil, "agent_errors returned nothing (rc=" .. tostring(rc) .. ")"
    end
    local ok, data = pcall(json.decode, raw)
    if not ok then
        return nil, "json.decode failed: " .. tostring(data)
    end
    return data
end

-- Pull a list of error entries out of the agent_errors payload.
-- We normalize to a flat table of { title, detail, raw }.
local function normalize(data)
    if not data then return {} end
    -- Common shapes the agent returns:
    --   { errors = [{ msg, ... }, ...] }
    --   { error = { ... } }              (single)
    --   { ok = true, errors = [] }       (no errors)
    local out = {}
    local list = data.errors
    if not list and data.error then list = { data.error } end
    if not list then return {} end

    for _, e in ipairs(list) do
        local title = e.msg or e.message or e.summary or e.title or "(unnamed error)"
        local where = e.file or e.path or e.location
        if where then
            title = where .. ": " .. title
        end
        out[#out + 1] = {
            title  = title,
            detail = e,
        }
    end
    return out
end

-- Pretty-print one error's detail panel.
local function detail_lines(entry, w)
    local lines = {}
    local function push(s)
        if #s > w then
            for i = 1, #s, w do
                lines[#lines + 1] = s:sub(i, i + w - 1)
            end
        else
            lines[#lines + 1] = s
        end
    end
    push(entry.title)
    push("")
    -- Dump known interesting fields first.
    local d = entry.detail or {}
    for _, key in ipairs({ "file", "path", "line", "column", "trace", "stack" }) do
        if d[key] then
            push(key .. ": " .. tostring(d[key]))
        end
    end
    -- Then the full JSON payload.
    push("")
    push("-- raw --")
    local ok, encoded = pcall(json.encode, d)
    if ok and encoded then
        for chunk in encoded:gmatch("[^\n]+") do push(chunk) end
    end
    return lines
end

local function render(t, state)
    t:clear()
    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, 1, "hull agent errors   " ..
                  #state.errors .. " error" ..
                  (#state.errors == 1 and "" or "s") ..
                  "   app_dir=" .. state.app_dir)
    t:style({})

    if state.message then
        t:style({ fg = 0xE06C75, bold = true })
        t:print(1, 3, "Error: " .. state.message)
        t:style({})
        t:style({ fg = 0x888888 })
        t:print(1, t.rows, "r:reload  q:quit")
        t:style({})
        return
    end

    if #state.errors == 0 then
        t:style({ fg = 0x00C000, bold = true })
        t:print(1, 3, "✓ No errors.")
        t:style({})
        t:style({ fg = 0x888888 })
        t:print(1, 5, "(.hull/last_error.json is empty - last reload succeeded.)")
        t:style({})
        t:style({ fg = 0x888888 })
        t:print(1, t.rows, "r:reload  q:quit")
        t:style({})
        return
    end

    -- Left pane: list of error titles.
    local left_w = math.min(48, math.floor(t.cols / 2))
    local h_list = t.rows - 3
    local top = state.top
    if state.cursor < top then top = state.cursor end
    if state.cursor > top + h_list - 1 then top = state.cursor - h_list + 1 end
    state.top = top

    for i = 0, h_list - 1 do
        local idx = top + i
        if idx > #state.errors then break end
        local e = state.errors[idx]
        local label = (idx == state.cursor and "> " or "  ") ..
                      e.title:sub(1, left_w - 3)
        if idx == state.cursor then
            t:style({ reverse = true })
            t:print(1, 3 + i, label .. string.rep(" ", left_w - #label))
            t:style({})
        else
            t:print(1, 3 + i, label)
        end
    end

    -- Separator.
    t:style({ fg = 0x444444 })
    for y = 2, t.rows - 1 do t:print(left_w + 1, y, "│") end
    t:style({})

    -- Right pane: detail of the cursor entry.
    local right_x = left_w + 3
    local right_w = math.max(20, t.cols - right_x)
    local lines = detail_lines(state.errors[state.cursor], right_w)
    local detail_h = t.rows - 3
    for i = 1, math.min(#lines, detail_h) do
        t:print(right_x, 2 + i, lines[i])
    end

    -- Key bar.
    t:style({ fg = 0x888888 })
    t:print(1, t.rows, "↑/↓:nav   r:reload   q:quit")
    t:style({})
end

-- ── Main ───────────────────────────────────────────────────────────

local function find_app_dir(args)
    for i = 2, #args do  -- arg[1] = "errors"; skip
        if args[i] and not args[i]:match("^%-") then
            return args[i]
        end
    end
    return "."
end

local app_dir = find_app_dir(arg or {})
local data, err = fetch(app_dir)

local state = {
    app_dir = app_dir,
    errors  = normalize(data),
    cursor  = 1,
    top     = 1,
    message = err,
}

tui.run({
    draw = function(t) render(t, state) end,
    on_event = function(ev)
        if ev.kind ~= "key" then return end
        local k = ev.key
        if k == "q" or k == "escape" or k == "ctrl+c" then return "quit"
        elseif k == "r" then
            local nd, nerr = fetch(app_dir)
            state.errors  = normalize(nd)
            state.message = nerr
            state.cursor  = math.max(1, math.min(state.cursor, #state.errors))
            state.top     = 1
        elseif k == "up" or k == "k" then
            if #state.errors > 0 then
                state.cursor = math.max(1, state.cursor - 1)
            end
        elseif k == "down" or k == "j" then
            if #state.errors > 0 then
                state.cursor = math.min(#state.errors, state.cursor + 1)
            end
        elseif k == "pageup" then
            state.cursor = math.max(1, state.cursor - 10)
        elseif k == "pagedown" then
            if #state.errors > 0 then
                state.cursor = math.min(#state.errors, state.cursor + 10)
            end
        elseif k == "home" then
            state.cursor = 1
        elseif k == "end" then
            state.cursor = #state.errors
        end
    end,
    tick_ms = -1,
})

-- Exit non-zero if there were errors so the TUI can be scripted in
-- CI-like flows: `hull agent errors --tui || exit 1` after a reload
-- check.
tool.exit((#state.errors == 0 and not state.message) and 0 or 1)
