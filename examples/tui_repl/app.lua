-- TUI REPL-style demo — Hull + Lua TUI example.
--
-- Demonstrates the tui.input building blocks: single-line editing,
-- arrow-key history navigation, ctrl+u/k for line kill. The "eval"
-- here is a tiny safe calculator (numeric expressions with +, -,
-- *, /, parens) — full Lua eval needs `load` which is sandboxed.
--
-- Run: hull run app.lua
-- Keys (at the input line):
--   enter          evaluate
--   ↑ / ↓          history (previous / next)
--   ← / →          cursor
--   home / end     jump
--   backspace      delete left
--   ctrl+u/k       kill to start/end
--   q / esc        quit (when input is empty)

local tui = require("hull.tui")

app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})

-- ── Tiny calculator parser (recursive descent) ────────────────────
--
-- Grammar (left-associative for + - * /):
--   expr   := term (("+"|"-") term)*
--   term   := factor (("*"|"/") factor)*
--   factor := number | "(" expr ")" | "-" factor

local function calc(src)
    local pos = 1
    local function skip() while pos <= #src and src:sub(pos, pos) == " " do pos = pos + 1 end end
    local function peek() skip(); return src:sub(pos, pos) end
    local function eat(c) if peek() == c then pos = pos + 1; return true end end
    local expr  -- forward decl

    local function number()
        skip()
        local s = pos
        while pos <= #src do
            local c = src:sub(pos, pos)
            if c:match("[%d%.]") then pos = pos + 1 else break end
        end
        if pos == s then error("expected number at column " .. s) end
        local n = tonumber(src:sub(s, pos - 1))
        if not n then error("invalid number at column " .. s) end
        return n
    end

    local function factor()
        if eat("(") then
            local v = expr()
            if not eat(")") then error("expected ')' at column " .. pos) end
            return v
        end
        if eat("-") then return -factor() end
        return number()
    end

    local function term()
        local v = factor()
        while true do
            local op = peek()
            if op == "*" then pos = pos + 1; v = v * factor()
            elseif op == "/" then
                pos = pos + 1
                local d = factor()
                if d == 0 then error("division by zero") end
                v = v / d
            else break end
        end
        return v
    end

    expr = function()
        local v = term()
        while true do
            local op = peek()
            if op == "+" then pos = pos + 1; v = v + term()
            elseif op == "-" then pos = pos + 1; v = v - term()
            else break end
        end
        return v
    end

    local ok, result = pcall(function()
        local v = expr()
        skip()
        if pos <= #src then
            error("unexpected '" .. src:sub(pos) .. "' at column " .. pos)
        end
        return v
    end)
    if not ok then return tostring(result), "error" end

    -- Format: integer if whole, else 6 decimals trimmed.
    local fmt = (result == math.floor(result))
                and string.format("%d", result)
                or string.format("%.6f", result):gsub("0+$", ""):gsub("%.$", "")
    return fmt, "value"
end

-- ── State ─────────────────────────────────────────────────────────

local history    = {}
local hist_cursor = 0
local hist_stash  = ""
local input_buf   = ""
local input_cur   = 1

-- ── Render ────────────────────────────────────────────────────────

local function render(t)
    t:clear()
    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, 1, "hull tui-repl (numeric calc)   " .. #history ..
                  " entries   q/esc on empty input to quit")
    t:style({})

    local input_y = t.rows - 1
    local out_h   = input_y - 3
    local visible = math.max(1, math.floor(out_h / 2))
    local start = math.max(1, #history - visible + 1)
    local y = 3
    for i = start, #history do
        if y > out_h + 2 then break end
        local h = history[i]
        t:style({ fg = 0x888888 })
        t:print(1, y, "> ")
        t:style({})
        t:print(3, y, h.input:sub(1, t.cols - 3))
        t:style({ fg = (h.kind == "error") and 0xE06C75 or 0x00C000 })
        t:print(3, y + 1, ("= " .. h.output):sub(1, t.cols - 3))
        t:style({})
        y = y + 2
    end

    t:style({ fg = 0x444444 })
    t:print(1, input_y - 1, string.rep("─", t.cols))
    t:style({})

    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, input_y, "calc> ")
    t:style({})
    t:print(7, input_y, input_buf)
    t:move(7 + input_cur - 1, input_y)
end

local function on_event(ev)
    if ev.kind ~= "key" then return end
    local k = ev.key

    if k == "enter" then
        if input_buf ~= "" then
            local out, kind = calc(input_buf)
            history[#history + 1] = { input = input_buf, output = out, kind = kind }
        end
        input_buf  = ""
        input_cur  = 1
        hist_cursor = 0
        hist_stash  = ""
    elseif k == "up" then
        if hist_cursor < #history then
            if hist_cursor == 0 then hist_stash = input_buf end
            hist_cursor = hist_cursor + 1
            input_buf = history[#history - hist_cursor + 1].input
            input_cur = #input_buf + 1
        end
    elseif k == "down" then
        if hist_cursor > 0 then
            hist_cursor = hist_cursor - 1
            input_buf = (hist_cursor == 0) and hist_stash
                       or history[#history - hist_cursor + 1].input
            input_cur = #input_buf + 1
        end
    elseif k == "left"  then input_cur = math.max(1, input_cur - 1)
    elseif k == "right" then input_cur = math.min(#input_buf + 1, input_cur + 1)
    elseif k == "home"  then input_cur = 1
    elseif k == "end"   then input_cur = #input_buf + 1
    elseif k == "backspace" then
        if input_cur > 1 then
            input_buf = input_buf:sub(1, input_cur - 2) .. input_buf:sub(input_cur)
            input_cur = input_cur - 1
        end
    elseif k == "ctrl+u" then
        input_buf = input_buf:sub(input_cur); input_cur = 1
    elseif k == "ctrl+k" then
        input_buf = input_buf:sub(1, input_cur - 1)
    elseif k == "ctrl+c" then
        return "quit"
    elseif (k == "q" or k == "escape") and input_buf == "" then
        return "quit"
    elseif ev.codepoint and ev.codepoint >= 0x20 and not ev.ctrl and not ev.alt then
        input_buf = input_buf:sub(1, input_cur - 1) .. ev.key .. input_buf:sub(input_cur)
        input_cur = input_cur + #ev.key
    end
end

app.main(function(ctx)
    tui.run({ draw = render, on_event = on_event, tick_ms = -1 })
    ctx.stdout:write("repl: " .. #history .. " expressions evaluated\n")
    return 0
end)
