-- hull._text - string helpers for the stdlib (internal: the underscore keeps
-- it out of app code).
--
-- trim / rtrim exist because the obvious patterns are quadratic. Both
-- s:match("^%s*(.-)%s*$") and s:gsub("%s+$", "") retry from every start
-- position inside a run of whitespace, so a run of n spaces with a
-- non-space after it costs O(n^2) - inside one C call, where the
-- instruction limit never fires. Request input reaches these trims (a Cookie
-- header, a form field under validate's trim rule, If-None-Match,
-- Accept-Language), and measured on a build: an 8 KB Cookie header held the
-- event loop for 0.7 s, a 32 KB form field for 5.9 s. These scan once.

local M = {}

local find, byte, sub = string.find, string.byte, string.sub

-- Lua's %s: space, \t, \n, \v, \f, \r.
local function is_space(b)
    return b == 32 or (b >= 9 and b <= 13)
end

--- s without leading and trailing whitespace. Linear.
function M.trim(s)
    local i = find(s, "%S")
    if not i then return "" end
    local j = #s
    while is_space(byte(s, j)) do j = j - 1 end
    return sub(s, i, j)
end

--- s without trailing whitespace. Linear.
function M.rtrim(s)
    local j = #s
    while j > 0 and is_space(byte(s, j)) do j = j - 1 end
    return sub(s, 1, j)
end

-- The Unicode white space JS's \s matches, as UTF-8 byte patterns (NEL,
-- NBSP, U+1680, U+2000-200A, U+2028 / 2029, U+202F, U+205F, U+3000, U+FEFF):
-- Lua's %s is ASCII only. Shared by the address checks so the Lua and JS
-- stdlibs refuse the same addresses (audits 20 / 21).
local UNICODE_SPACE = {
    "\xC2[\x85\xA0]", "\xE1\x9A\x80", "\xE2\x80[\x80-\x8A\xA8\xA9\xAF]",
    "\xE2\x81\x9F", "\xE3\x80\x80", "\xEF\xBB\xBF",
}

-- A C1 control character (U+0080-009F, encoded \xC2\x80-\xC2\x9F; NEL is
-- U+0085): the address checks refuse them with the C0 ones, as the JS
-- twins do (audit 26).
function M.has_c1_control(s)
    return s:find("\xC2[\x80-\x9F]") ~= nil
end

function M.has_unicode_space(s)
    for _, sp in ipairs(UNICODE_SPACE) do
        if s:find(sp) then return true end
    end
    return false
end

-- @p s cut to at most @p n bytes without splitting a UTF-8 sequence: a
-- byte cut mid-character stored invalid UTF-8, which Postgres and a strict
-- MySQL utf8mb4 column refuse (audit 21).
function M.utf8_cut(s, n)
    if #s <= n then return s end
    s = s:sub(1, n)
    for i = #s, math.max(1, #s - 3), -1 do
        local b = s:byte(i)
        if b < 0x80 then return s end
        if b >= 0xC0 then
            local need = b >= 0xF0 and 4 or (b >= 0xE0 and 3 or 2)
            if #s - i + 1 >= need then return s end
            return s:sub(1, i - 1)
        end
    end
    return s
end

return M
