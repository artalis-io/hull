-- stdlib/lua/hull/modules_available_tui.lua — searchable module registry.
--
-- Run via `hull modules available --tui`. Two-pane: scrollable list
-- of every first-party module on the left, detail panel (deps,
-- caps, manifest field requirements) on the right.
--
-- Keys
--   /              open the filter prompt (substring match on name)
--   esc            cancel filter
--   ↑/↓ ←/→ j/k    navigate
--   PgUp/PgDn      jump 10
--   Home/End       jump to top/bottom
--   q              quit
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local tui = require("hull.tui")

local function colors_for_theme(theme)
    if theme == "light" then
        return { ok = 0x006400, warn = 0xB8860B, err = 0x8B0000,
                 dim = 0x666666, head = 0x000080 }
    end
    return { ok = 0x00C000, warn = 0xE5C07B, err = 0xE06C75,
             dim = 0x888888, head = 0x61AFEF }
end

-- ── Data ──────────────────────────────────────────────────────────

local data = tool.modules_available()
local all  = data.modules

local function filter(query)
    if not query or query == "" then return all end
    local out, n = {}, 0
    for _, m in ipairs(all) do
        if m.name:find(query, 1, true) then
            n = n + 1
            out[n] = m
        end
    end
    return out
end

-- ── Render ────────────────────────────────────────────────────────

local state = {
    cursor   = 1,
    top      = 1,
    query    = "",
    query_edit = false,
    qcursor  = 1,
    visible  = all,
    palette  = colors_for_theme(tui.theme()),
}

local function render_list(t, left_w)
    local pal = state.palette
    local list = state.visible
    local h = t.rows - 3
    if state.cursor < state.top then state.top = state.cursor end
    if state.cursor > state.top + h - 1 then state.top = state.cursor - h + 1 end
    if #list == 0 then
        t:style({ fg = pal.dim, italic = true })
        t:print(1, 3, "(no matches)")
        t:style({})
        return
    end
    for i = 0, h - 1 do
        local idx = state.top + i
        if idx > #list then break end
        local m = list[idx]
        local label = (idx == state.cursor and "> " or "  ") .. m.name
        if #label > left_w then label = label:sub(1, left_w) end
        if idx == state.cursor then
            t:style({ reverse = true })
            t:print(1, 3 + i, label .. string.rep(" ", left_w - #label))
            t:style({})
        else
            t:print(1, 3 + i, label)
        end
    end
end

local function render_detail(t, x, w)
    local list = state.visible
    if #list == 0 then return end
    local m = list[state.cursor]
    local pal = state.palette
    local y = 3
    t:style({ bold = true, fg = pal.head })
    t:print(x, y, m.name .. "@" .. m.api_major)
    t:style({})
    y = y + 1

    local tags = {}
    if m.intrinsic then tags[#tags + 1] = "intrinsic" end
    if m.pure      then tags[#tags + 1] = "pure"     end
    if not m.intrinsic then tags[#tags + 1] = "must declare" end
    if #tags > 0 then
        t:style({ fg = pal.dim })
        t:print(x, y, table.concat(tags, ", "))
        t:style({})
        y = y + 1
    end
    y = y + 1

    -- Caps
    t:style({ bold = true })
    t:print(x, y, "required caps:")
    t:style({})
    y = y + 1
    if #m.caps == 0 then
        t:style({ fg = pal.dim })
        t:print(x + 2, y, "(none)")
        t:style({})
        y = y + 1
    else
        for _, c in ipairs(m.caps) do
            t:print(x + 2, y, "• " .. c)
            y = y + 1
        end
    end
    y = y + 1

    -- Deps
    t:style({ bold = true })
    t:print(x, y, "deps:")
    t:style({})
    y = y + 1
    if #m.deps == 0 then
        t:style({ fg = pal.dim })
        t:print(x + 2, y, "(none)")
        t:style({})
        y = y + 1
    else
        for _, d in ipairs(m.deps) do
            t:print(x + 2, y, "• " .. d)
            y = y + 1
        end
    end
    y = y + 1

    -- Usage hint.
    if not m.intrinsic then
        t:style({ fg = pal.dim, italic = true })
        local hint = string.format(
            'app.manifest({ modules = { "%s@%d" } })',
            m.name, m.api_major)
        if #hint > w then hint = hint:sub(1, w) end
        t:print(x, y, hint)
        t:style({})
    end
end

local function render(t)
    t:clear()
    local pal = state.palette

    -- Title.
    t:style({ bold = true, fg = pal.head })
    t:print(1, 1, "hull modules available   " .. #state.visible ..
                  " / " .. #all .. " modules")
    t:style({})

    local left_w = math.min(32, math.floor(t.cols * 0.4))

    render_list(t, left_w)

    -- Separator.
    t:style({ fg = 0x444444 })
    for y = 2, t.rows - 1 do
        t:print(left_w + 1, y, "│")
    end
    t:style({})

    -- Detail.
    render_detail(t, left_w + 3, t.cols - left_w - 3)

    -- Bottom bar.
    if state.query_edit then
        t:style({ fg = pal.head, bold = true })
        t:print(1, t.rows, "filter: ")
        t:style({})
        t:print(9, t.rows, state.query)
        t:move(9 + state.qcursor - 1, t.rows)
    else
        t:style({ fg = pal.dim })
        local bar = "↑/↓:nav   /:filter   q:quit"
        if state.query ~= "" then
            bar = bar .. "    filter=\"" .. state.query .. "\""
        end
        t:print(1, t.rows, bar)
        t:style({})
    end
end

-- ── Event handling ────────────────────────────────────────────────

local function on_event(ev)
    if ev.kind ~= "key" then return end
    local k = ev.key

    if state.query_edit then
        if k == "enter" then
            state.query_edit = false
            state.visible = filter(state.query)
            state.cursor = 1; state.top = 1
        elseif k == "escape" then
            state.query_edit = false
            state.query = ""
            state.visible = all
            state.cursor = 1; state.top = 1
        elseif k == "backspace" then
            if state.qcursor > 1 then
                state.query = state.query:sub(1, state.qcursor - 2)
                              .. state.query:sub(state.qcursor)
                state.qcursor = state.qcursor - 1
                state.visible = filter(state.query)
                state.cursor = 1; state.top = 1
            end
        elseif ev.codepoint and ev.codepoint >= 0x20 and not ev.ctrl and not ev.alt then
            state.query = state.query:sub(1, state.qcursor - 1)
                          .. ev.key
                          .. state.query:sub(state.qcursor)
            state.qcursor = state.qcursor + #ev.key
            state.visible = filter(state.query)
            state.cursor = 1; state.top = 1
        end
        return
    end

    if k == "q" or k == "ctrl+c" then return "quit"
    elseif k == "/" or k == "f" then
        state.query_edit = true
        state.qcursor = #state.query + 1
    elseif k == "up" or k == "k" then
        if #state.visible > 0 then
            state.cursor = math.max(1, state.cursor - 1)
        end
    elseif k == "down" or k == "j" then
        if #state.visible > 0 then
            state.cursor = math.min(#state.visible, state.cursor + 1)
        end
    elseif k == "pageup" then
        state.cursor = math.max(1, state.cursor - 10)
    elseif k == "pagedown" then
        if #state.visible > 0 then
            state.cursor = math.min(#state.visible, state.cursor + 10)
        end
    elseif k == "home" then
        state.cursor = 1
    elseif k == "end" then
        state.cursor = #state.visible
    elseif k == "escape" then
        if state.query ~= "" then
            state.query = ""
            state.visible = all
            state.cursor = 1; state.top = 1
        else
            return "quit"
        end
    end
end

tui.run({
    draw = render,
    on_event = on_event,
    tick_ms = -1,
})

tool.exit(0)
