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

return M
