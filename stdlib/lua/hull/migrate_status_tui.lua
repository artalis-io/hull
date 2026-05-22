-- stdlib/lua/hull/migrate_status_tui.lua — visual migration ledger.
--
-- Invoked by `hull migrate status --tui`. Two-pane: list of every
-- discovered migration on the left, source preview (the .sql file
-- content) on the right. ↑/↓ to scroll, r to re-query, q to quit.
--
-- The data comes from tool.migrate_status(app_dir, db_path) — a
-- direct wrapper around the C hl_migrate_status path. We read the
-- .sql source via tool.read_file when the user focuses an entry.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local tui = require("hull.tui")

-- ── Arg parsing (matches commands/migrate.c::cmd_status) ─────────

local function find_args(args)
    local app_dir = "."
    local db_path = "data.db"
    local i = 2  -- arg[1] = "status"
    while args[i] do
        local a = args[i]
        if a == "-d" and args[i + 1] then
            db_path = args[i + 1]; i = i + 2
        elseif a:sub(1, 1) == "-" then
            i = i + 1
        else
            app_dir = a; i = i + 1
        end
    end
    return app_dir, db_path
end

local app_dir, db_path = find_args(arg or {})

-- ── Probe ─────────────────────────────────────────────────────────

local function probe()
    local entries, err_or_count = tool.migrate_status(app_dir, db_path)
    if not entries then
        return nil, tostring(err_or_count or "unknown error")
    end
    return entries
end

-- ── Source preview cache ──────────────────────────────────────────

local source_cache = {}

local function read_source(name)
    if source_cache[name] then return source_cache[name] end
    local path = app_dir .. "/migrations/" .. name
    local ok, contents = pcall(tool.read_file, path)
    if not ok or not contents then
        source_cache[name] = nil
        return nil
    end
    source_cache[name] = contents
    return contents
end

-- ── State ─────────────────────────────────────────────────────────

local state = {
    entries = nil,
    error   = nil,
    cursor  = 1,
    top     = 1,
    palette = (tui.theme() == "light")
              and { ok = 0x006400, err = 0x8B0000, dim = 0x666666,
                    head = 0x000080, warn = 0xB8860B }
              or  { ok = 0x00C000, err = 0xE06C75, dim = 0x888888,
                    head = 0x61AFEF, warn = 0xE5C07B },
}

local function reload()
    state.entries, state.error = probe()
    state.cursor = math.max(1, math.min(state.cursor, #(state.entries or {})))
end

reload()

-- ── Render ────────────────────────────────────────────────────────

local function applied_count()
    local n = 0
    for _, e in ipairs(state.entries or {}) do
        if e.applied then n = n + 1 end
    end
    return n
end

local function render(t)
    t:clear()

    -- Title.
    local entries = state.entries or {}
    local applied = applied_count()
    local pending = #entries - applied
    local pal = state.palette

    t:style({ bold = true, fg = pal.head })
    t:print(1, 1, "hull migrate status   " ..
                  applied .. " applied, " ..
                  pending .. " pending   db=" .. db_path)
    t:style({})

    if state.error then
        t:style({ fg = pal.err, bold = true })
        t:print(1, 3, "Error: " .. state.error)
        t:style({})
        t:style({ fg = pal.dim, italic = true })
        t:print(1, 5, "press 'r' to retry after creating the DB or migrations dir")
        t:style({})
        t:style({ fg = pal.dim })
        t:print(1, t.rows, "r:retry  q:quit")
        t:style({})
        return
    end

    if #entries == 0 then
        t:style({ fg = pal.dim, italic = true })
        t:print(1, 3, "(no migrations found in " .. app_dir .. "/migrations/)")
        t:style({})
        t:style({ fg = pal.dim })
        t:print(1, t.rows, "r:retry  q:quit")
        t:style({})
        return
    end

    -- Left pane: list.
    local left_w = math.min(48, math.floor(t.cols * 0.5))
    local h = t.rows - 3
    if state.cursor < state.top then state.top = state.cursor end
    if state.cursor > state.top + h - 1 then state.top = state.cursor - h + 1 end

    for i = 0, h - 1 do
        local idx = state.top + i
        if idx > #entries then break end
        local e = entries[idx]
        local mark = e.applied and "[x]" or "[ ]"
        local label = (idx == state.cursor and "> " or "  ") ..
                      mark .. " " .. e.name
        if #label > left_w then label = label:sub(1, left_w) end
        if idx == state.cursor then
            t:style({ reverse = true,
                      fg = e.applied and pal.ok or pal.warn })
            t:print(1, 3 + i, label .. string.rep(" ", left_w - #label))
            t:style({})
        else
            t:style({ fg = e.applied and pal.ok or pal.warn })
            t:print(1, 3 + i, label)
            t:style({})
        end
    end

    -- Separator.
    t:style({ fg = 0x444444 })
    for y = 2, t.rows - 1 do t:print(left_w + 1, y, "│") end
    t:style({})

    -- Right pane: detail.
    local right_x = left_w + 3
    local right_w = math.max(20, t.cols - right_x)
    local sel = entries[state.cursor]

    t:style({ bold = true, fg = pal.head })
    t:print(right_x, 3, sel.name)
    t:style({})

    if sel.applied then
        t:style({ fg = pal.ok })
        t:print(right_x, 4, "applied at " .. (sel.applied_at or "?"))
        t:style({})
    else
        t:style({ fg = pal.warn, bold = true })
        t:print(right_x, 4, "pending — will run on next `hull migrate`")
        t:style({})
    end

    local src = read_source(sel.name)
    if src then
        t:style({ fg = pal.dim })
        t:print(right_x, 6, "── source ──")
        t:style({})
        local body_y = 7
        local body_h = t.rows - body_y - 1
        local line_n = 0
        for line in (src .. "\n"):gmatch("([^\n]*)\n") do
            if line_n >= body_h then break end
            if #line > right_w then line = line:sub(1, right_w) end
            t:print(right_x, body_y + line_n, line)
            line_n = line_n + 1
        end
    else
        t:style({ fg = pal.dim, italic = true })
        t:print(right_x, 6, "(source unavailable: " ..
                            app_dir .. "/migrations/" .. sel.name .. ")")
        t:style({})
    end

    -- Key bar.
    t:style({ fg = pal.dim })
    t:print(1, t.rows,
            "↑/↓:nav   pgup/pgdn   r:reload   q:quit")
    t:style({})
end

-- ── Event handling ────────────────────────────────────────────────

local function on_event(ev)
    if ev.kind ~= "key" then return end
    local k = ev.key
    if k == "q" or k == "escape" or k == "ctrl+c" then return "quit"
    elseif k == "r" then
        source_cache = {}
        reload()
    elseif k == "up" or k == "k" then
        if state.entries and #state.entries > 0 then
            state.cursor = math.max(1, state.cursor - 1)
        end
    elseif k == "down" or k == "j" then
        if state.entries and #state.entries > 0 then
            state.cursor = math.min(#state.entries, state.cursor + 1)
        end
    elseif k == "pageup" then
        state.cursor = math.max(1, state.cursor - 10)
    elseif k == "pagedown" and state.entries then
        state.cursor = math.min(#state.entries, state.cursor + 10)
    elseif k == "home" then state.cursor = 1
    elseif k == "end" and state.entries then
        state.cursor = #state.entries
    end
end

tui.run({
    draw     = render,
    on_event = on_event,
    tick_ms  = -1,
})

tool.exit(state.error and 1 or 0)
