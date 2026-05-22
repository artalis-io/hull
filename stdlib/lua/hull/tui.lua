-- stdlib/lua/hull/tui.lua — TUI helper module on top of cap-layer
-- primitives.
--
-- `require("hull.tui")` gives back the *native* hull.tui table
-- (registered in src/hull/runtime/lua/modules.c) plus a few pure-Lua
-- helpers added in this file. The native bindings expose acquire /
-- release / size / theme / clear / move / style / print / write /
-- flush / poll / clipboard_set / enable_* / async; the Lua side adds
-- tui.run and a couple of convenience widgets.
--
-- The canonical entry point is `tui.run({ draw, on_event, tick_ms })`.
-- The raw primitives are exposed but should rarely be needed.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

-- Module-load constants (used internally; not part of the API).
local TUI_RUN_DEFAULT_TICK_MS = -1   -- block until event by default
local TUI_RUN_MAX_DEFER_FRAMES = 60  -- safety cap on deferred redraw

-- Bridge to the native bindings. The stdlib *is* the public `hull.tui`
-- module — augment the C primitives with run-loop and widget helpers.
local tui = require("hull._tui")

-- ── Internal ───────────────────────────────────────────────────

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- A draw-context handed to the user's `draw` function. Holds
-- current size + a few convenience methods that delegate to the
-- module-level cap functions. Stable across the lifetime of a
-- single tui.run call.
--
-- Methods accept method-call syntax (`t:print(...)`) by swallowing
-- the implicit `self` argument; the underlying cap bindings take
-- positional arguments without `self`.
local function make_draw_ctx()
    local cols, rows = tui.size()
    return {
        cols = cols,
        rows = rows,
        clear      = function(_)            return tui.clear()           end,
        invalidate = function(_)            return tui.invalidate()      end,
        move       = function(_, x, y)      return tui.move(x, y)        end,
        style      = function(_, opts)      return tui.style(opts)       end,
        print      = function(_, x, y, s)   return tui.print(x, y, s)    end,
        write      = function(_, s)         return tui.write(s)          end,
        flush      = function(_)            return tui.flush()           end,
        theme      = function(_)            return tui.theme()           end,
    }
end

-- Refresh dims on resize. Mutates the existing ctx so user
-- references to `t.cols` / `t.rows` see new values without needing
-- to re-grab a context.
local function refresh_dims(ctx)
    local c, r = tui.size()
    ctx.cols = c
    ctx.rows = r
end

-- ── tui.run ────────────────────────────────────────────────────
--
-- Immediate-mode loop. opts = {
--     draw     = function(t) ... end,         -- required
--     on_event = function(ev) ... return nil|exit_token end,
--     tick_ms  = -1 | 0 | N,                  -- default: -1 (block)
--     mouse    = false,
--     paste    = false,                       -- default: off
--     focus    = false,
--     kitty_kbd= false,
--     allow_no_tty = false,                   -- skip if not a tty
-- }
--
-- Returns whatever `on_event` returned (typically "done" / "abort")
-- or "done" if `draw` raised an error after cleanup (the error is
-- re-raised so the caller sees it).

function tui.run(opts)
    assert(type(opts) == "table", "tui.run: opts must be a table")
    local draw = opts.draw
    assert(type(draw) == "function", "tui.run: opts.draw must be a function")
    local on_event = opts.on_event   -- optional
    local tick_ms  = opts.tick_ms or TUI_RUN_DEFAULT_TICK_MS

    tui.enter()

    if opts.mouse     then tui.enable_mouse(true) end
    if opts.paste     then tui.enable_paste(true) end
    if opts.focus     then tui.enable_focus(true) end
    if opts.kitty_kbd then tui.enable_kitty_kbd(true) end

    local ctx = make_draw_ctx()
    local exit_token = nil
    local has_exit   = false   -- separate flag: exit_token can be false

    -- Render once before waiting for the first event so the user
    -- sees the UI immediately.
    local ok, err = pcall(draw, ctx)
    if ok then
        tui.flush()
    else
        tui.leave()
        error(err, 0)
    end

    while not has_exit do
        local ev = tui.poll(tick_ms)
        if ev then
            if ev.kind == "resize" then
                refresh_dims(ctx)
            end
            if on_event then
                local eok, r = pcall(on_event, ev)
                if not eok then
                    tui.leave()
                    error(r, 0)
                end
                if r ~= nil then
                    exit_token = r
                    has_exit = true
                end
            end
        end
        if not has_exit then
            ok, err = pcall(draw, ctx)
            if not ok then
                tui.leave()
                error(err, 0)
            end
            tui.flush()
        end
    end

    tui.leave()
    return exit_token
end

-- ── tui.list ───────────────────────────────────────────────────
--
-- Single-column scrollable picker. Returns the picked index (1-based)
-- or nil if the user aborted (ESC / q / ctrl+c).
--
-- opts = { title = "Pick one", initial = 1, height = nil }

function tui.list(items, opts)
    assert(type(items) == "table", "tui.list: items must be a table")
    opts = opts or {}
    local title = opts.title
    local count = #items
    if count == 0 then return nil end

    local cursor = clamp(opts.initial or 1, 1, count)
    local top = 1   -- index of the first visible item

    local function fit_height(ctx)
        local h = opts.height or ctx.rows - (title and 2 or 0)
        return clamp(h, 1, count)
    end

    return tui.run({
        draw = function(t)
            t:clear()
            local y = 1
            if title then
                t:style({ bold = true })
                t:print(1, y, title)
                t:style({})
                y = y + 1
            end
            local h = fit_height(t)
            -- Keep cursor visible.
            if cursor < top then top = cursor end
            if cursor > top + h - 1 then top = cursor - h + 1 end
            for i = 0, h - 1 do
                local idx = top + i
                if idx > count then break end
                if idx == cursor then
                    t:style({ reverse = true })
                    t:print(1, y + i, "> " .. tostring(items[idx]))
                    t:style({})
                else
                    t:print(1, y + i, "  " .. tostring(items[idx]))
                end
            end
        end,
        on_event = function(ev)
            if ev.kind ~= "key" then return end
            local k = ev.key
            if k == "up"   or k == "k" then cursor = clamp(cursor - 1, 1, count)
            elseif k == "down" or k == "j" then cursor = clamp(cursor + 1, 1, count)
            elseif k == "pageup"   then cursor = clamp(cursor - 10, 1, count)
            elseif k == "pagedown" then cursor = clamp(cursor + 10, 1, count)
            elseif k == "home" then cursor = 1
            elseif k == "end"  then cursor = count
            elseif k == "enter" then return cursor
            elseif k == "escape" or k == "q" or k == "ctrl+c" then return false
            end
        end,
    })
end

-- ── tui.confirm ────────────────────────────────────────────────

function tui.confirm(msg)
    return tui.run({
        draw = function(t)
            t:clear()
            t:print(1, 1, tostring(msg) .. " [y/N]")
        end,
        on_event = function(ev)
            if ev.kind ~= "key" then return end
            if ev.key == "y" or ev.key == "Y" then return true end
            if ev.key == "n" or ev.key == "N" then return false end
            if ev.key == "enter" then return false end
            if ev.key == "escape" or ev.key == "ctrl+c" then return false end
        end,
    }) and true or false
end

-- ── tui.frame ──────────────────────────────────────────────────
--
-- Draw a single-line box at the given rect and call `fn(inner)` with
-- a sub-context that has the inset dimensions and a `print` that
-- maps (1,1) to the first cell inside the border. Wraps drawing
-- only — the caller still has to call `tui.flush` (or be inside a
-- `tui.run` cycle which does that automatically).
--
-- opts = { x = 1, y = 1, w = cols, h = rows, title = nil, border = "single" }
--
-- Border characters use Unicode line-drawing; "ascii" is available
-- for terminals that don't render box characters cleanly.

local BORDERS = {
    single = { tl = "┌", tr = "┐", bl = "└", br = "┘", h = "─", v = "│" },
    double = { tl = "╔", tr = "╗", bl = "╚", br = "╝", h = "═", v = "║" },
    round  = { tl = "╭", tr = "╮", bl = "╰", br = "╯", h = "─", v = "│" },
    ascii  = { tl = "+", tr = "+", bl = "+", br = "+", h = "-", v = "|" },
}

function tui.frame(opts, fn)
    assert(type(opts) == "table", "tui.frame: opts table required")
    local cols, rows = tui.size()
    local x = opts.x or 1
    local y = opts.y or 1
    local w = opts.w or (cols - x + 1)
    local h = opts.h or (rows - y + 1)
    local title = opts.title
    local style = opts.style    -- forwarded to tui.style for the border
    local kind  = opts.border or "single"
    local b     = BORDERS[kind] or BORDERS.single

    if w < 2 or h < 2 then return end   -- nothing to draw

    if style then tui.style(style) end

    -- Top edge
    local top = b.tl .. string.rep(b.h, w - 2) .. b.tr
    if title and #title > 0 and w - 4 > 0 then
        local tt = title
        if #tt > w - 4 then tt = string.sub(tt, 1, w - 4) end
        -- Slot the title into the top edge: ┌─ title ─...─┐
        local prefix = b.tl .. b.h .. " "
        local suffix_len = w - #prefix - #tt - 2
        if suffix_len < 0 then suffix_len = 0 end
        top = prefix .. tt .. " " .. string.rep(b.h, suffix_len) .. b.tr
    end
    tui.print(x, y, top)

    -- Side edges
    for i = 1, h - 2 do
        tui.print(x,           y + i, b.v)
        tui.print(x + w - 1,   y + i, b.v)
    end

    -- Bottom edge
    tui.print(x, y + h - 1, b.bl .. string.rep(b.h, w - 2) .. b.br)

    if style then tui.style({}) end

    if fn then
        local inner = {
            x = x + 1,
            y = y + 1,
            w = w - 2,
            h = h - 2,
            print = function(_, ix, iy, s)
                if ix < 1 or iy < 1 or ix > w - 2 or iy > h - 2 then return end
                -- Truncate to inner width.
                if #s > w - 2 - (ix - 1) then s = string.sub(s, 1, w - 2 - (ix - 1)) end
                tui.print(x + ix, y + iy, s)
            end,
        }
        fn(inner)
    end
end

-- ── tui.input ──────────────────────────────────────────────────
--
-- Single-line text input. Editing keys: left/right move cursor,
-- home/end jump, backspace deletes, delete forwards. Returns the
-- entered string on enter, nil on escape/ctrl+c.
--
-- opts = { initial = "", prompt = "", max_len = 1024, password = false }

function tui.input(prompt, opts)
    opts = opts or {}
    local initial = opts.initial or ""
    local max_len = opts.max_len or 1024
    local password = opts.password and true or false
    local prompt_str = prompt or ""

    local buf = initial
    local cursor = #buf + 1   -- 1-based, points at character to the right of cursor

    return tui.run({
        draw = function(t)
            t:clear()
            t:print(1, 1, prompt_str)
            local display
            if password then
                display = string.rep("*", #buf)
            else
                display = buf
            end
            local x = #prompt_str + 1
            t:print(x, 1, display)
            -- Show the cursor by moving there; the terminal's actual
            -- cursor follows tui.move via the flush.
            t:move(x + cursor - 1, 1)
        end,
        on_event = function(ev)
            if ev.kind ~= "key" then return end
            local k = ev.key
            if k == "enter" then return buf
            elseif k == "escape" or k == "ctrl+c" then return false
            elseif k == "backspace" then
                if cursor > 1 then
                    buf = string.sub(buf, 1, cursor - 2) .. string.sub(buf, cursor)
                    cursor = cursor - 1
                end
            elseif k == "delete" then
                if cursor <= #buf then
                    buf = string.sub(buf, 1, cursor - 1) .. string.sub(buf, cursor + 1)
                end
            elseif k == "left" then
                if cursor > 1 then cursor = cursor - 1 end
            elseif k == "right" then
                if cursor <= #buf then cursor = cursor + 1 end
            elseif k == "home" then
                cursor = 1
            elseif k == "end" then
                cursor = #buf + 1
            elseif k == "ctrl+u" then
                buf = string.sub(buf, cursor)
                cursor = 1
            elseif k == "ctrl+k" then
                buf = string.sub(buf, 1, cursor - 1)
            elseif ev.codepoint and ev.codepoint >= 0x20 and not ev.ctrl and not ev.alt then
                if #buf < max_len then
                    -- Encode codepoint as UTF-8 (use the key string
                    -- which already has it encoded).
                    local ch = ev.key
                    buf = string.sub(buf, 1, cursor - 1) .. ch .. string.sub(buf, cursor)
                    cursor = cursor + #ch
                end
            end
        end,
    }) or nil   -- false → nil (aborted)
end

-- ── tui.spinner ────────────────────────────────────────────────

local SPINNER_FRAMES = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" }
function tui.spinner(state)
    state = (state or 0) + 1
    if state > #SPINNER_FRAMES then state = 1 end
    return SPINNER_FRAMES[state], state
end

-- ── tui.progress ───────────────────────────────────────────────
--
-- Render a progress bar string. `pct` ∈ [0, 1]. `width` defaults to
-- 20 columns of bar plus brackets + a 4-char "%NNN" suffix.

function tui.progress(pct, opts)
    opts = opts or {}
    local width = opts.width or 20
    pct = clamp(pct or 0, 0, 1)
    local fill = math.floor(pct * width + 0.5)
    local bar = string.rep("█", fill) .. string.rep("░", width - fill)
    return string.format("[%s] %3d%%", bar, math.floor(pct * 100 + 0.5))
end

return tui
