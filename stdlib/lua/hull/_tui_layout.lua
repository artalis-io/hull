-- hull._tui_layout - the pure layout and line-editing helpers behind
-- hull.tui (internal: the underscore keeps it out of app code).
--
-- Split out of tui.lua so the arithmetic can be tested without a terminal:
-- hull.tui itself needs the native terminal bindings, which a TUI-free build
-- does not have. Nothing here touches the terminal. The JS twin is
-- hull:_tui_layout; both count text by code point (one cell each - the cap
-- layer's East Asian width table is not exposed to scripts, so a wide
-- character still counts as one).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local M = {}

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end
M.clamp = clamp

-- A codepoint is "printable input" if it isn't a C0 control byte
-- (< 0x20), DEL (0x7F), or a C1 control (0x80..0x9F). Used by
-- tui.input to reject keystrokes that would otherwise insert
-- terminal-escape bytes into the returned string.
function M.is_printable_cp(cp)
    if not cp then return false end
    if cp < 0x20 then return false end
    if cp == 0x7F then return false end
    if cp >= 0x80 and cp <= 0x9F then return false end
    return true
end

-- The columns `s` takes: its code points, or its bytes when it is not
-- valid UTF-8 (each malformed byte is drawn as one replacement cell).
local function text_cells(s)
    return utf8.len(s) or #s
end
M.text_cells = text_cells

-- UTF-8-safe truncate: trim `s` to at most `max_cells` code points
-- WITHOUT cutting a multi-byte codepoint in half.
function M.truncate(s, max_cells)
    if max_cells <= 0 then return "" end
    if utf8.len(s) and utf8.len(s) <= max_cells then return s end
    local last = utf8.offset(s, max_cells + 1)
    if not last then return s end
    return string.sub(s, 1, last - 1)
end

-- The top edge of a frame `w` cells wide, with `title` slotted in:
-- ┌─ title ─...─┐. The framing takes five cells (tl + h + " " before the
-- title, " " + tr after), so the title gets at most w-5. The fill is
-- counted in cells: counted in bytes, the 3-byte Unicode border pieces made
-- the edge 4 cells short, and more with a non-ASCII title (audit 44).
function M.frame_top(b, w, title)
    local top = b.tl .. string.rep(b.h, w - 2) .. b.tr
    if title and #title > 0 and w - 5 > 0 then
        local tt = M.truncate(title, w - 5)
        local prefix = b.tl .. b.h .. " "
        local fill = w - text_cells(prefix) - text_cells(tt) - 2
        if fill < 0 then fill = 0 end
        top = prefix .. tt .. " " .. string.rep(b.h, fill) .. b.tr
    end
    return top
end

-- Rows tui.list shows items in: the terminal less the one title row
-- (it took two - audit 44), or opts.height; at least 1, at most `count`.
function M.list_height(rows, title, height, count)
    local h = height or rows - (title and 1 or 0)
    return clamp(h, 1, count)
end

-- ── tui.input editing ──────────────────────────────────────────
--
-- Editing is by codepoint, not byte: the cursor is a byte index (1-based,
-- at the character right of the cursor) that always sits on a character
-- boundary. `max_len` counts code points (it counted bytes; the JS twin
-- counted UTF-16 units - audit 44).

local function is_cont(s, i)
    local b = string.byte(s, i)
    return b ~= nil and b >= 0x80 and b < 0xC0
end

-- Byte index where the character before byte position `i` starts.
local function prev_char(s, i)
    local j = i - 1
    while j > 1 and is_cont(s, j) do j = j - 1 end
    return j
end

-- Byte index just past the character that starts at `i`.
local function next_char(s, i)
    local j = i + 1
    while j <= #s and is_cont(s, j) do j = j + 1 end
    return j
end

-- Characters (codepoints) in s[1 .. i-1]; a malformed byte counts as one.
local function chars_before(s, i)
    local n, j = 0, 1
    while j < i do
        n = n + 1
        j = next_char(s, j)
    end
    return n
end
M.chars_before = chars_before

-- One key event applied to the line: returns buf, cursor - or, when the
-- edit ends, nil, nil, result (the text on enter, false on escape).
function M.input_step(buf, cursor, ev, max_len)
    local k = ev.key
    if k == "enter" then return nil, nil, buf
    elseif k == "escape" or k == "ctrl+c" then return nil, nil, false
    elseif k == "backspace" then
        if cursor > 1 then
            local p = prev_char(buf, cursor)
            buf = string.sub(buf, 1, p - 1) .. string.sub(buf, cursor)
            cursor = p
        end
    elseif k == "delete" then
        if cursor <= #buf then
            buf = string.sub(buf, 1, cursor - 1)
                  .. string.sub(buf, next_char(buf, cursor))
        end
    elseif k == "left" then
        if cursor > 1 then cursor = prev_char(buf, cursor) end
    elseif k == "right" then
        if cursor <= #buf then cursor = next_char(buf, cursor) end
    elseif k == "home" then
        cursor = 1
    elseif k == "end" then
        cursor = #buf + 1
    elseif k == "ctrl+u" then
        buf = string.sub(buf, cursor)
        cursor = 1
    elseif k == "ctrl+k" then
        buf = string.sub(buf, 1, cursor - 1)
    elseif M.is_printable_cp(ev.codepoint) and not ev.ctrl and not ev.alt then
        -- ev.key already holds the codepoint UTF-8 encoded; is_printable_cp
        -- has filtered C0 / DEL / C1, so it carries no escape sequence.
        local ch = ev.key
        if type(ch) == "string" and chars_before(buf, #buf + 1) < max_len then
            buf = string.sub(buf, 1, cursor - 1) .. ch .. string.sub(buf, cursor)
            cursor = cursor + #ch
        end
    end
    return buf, cursor
end

-- What tui.input draws: the (masked) text, its column, the cursor column.
function M.input_view(prompt_str, buf, cursor, password)
    local display = buf
    if password then
        display = string.rep("*", chars_before(buf, #buf + 1))
    end
    local x = chars_before(prompt_str, #prompt_str + 1) + 1
    return display, x, x + chars_before(buf, cursor)
end

return M
