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

-- App, request or remote text as text every database backend stores
-- (audits 38 / 39). Postgres refuses a NUL and bytes that are not UTF-8,
-- MySQL's utf8mb4 columns refuse invalid UTF-8 and values over the column
-- (64 KiB for TEXT), and the stdlib writes such text - a User-Agent with
-- obs-text bytes, a handler's error, a filename, a remote error body - after
-- a step it cannot undo (a session rotated, a single-use token consumed, a
-- message delivered). A refused write there wedged the row or failed the
-- request after the side effect. So, for any value:
--   * a non-string is converted under pcall (an __tostring is app code that
--     may raise; audit 23), and one that cannot be converted becomes
--     @p unconvertible (default M.UNCONVERTIBLE);
--   * NULs are dropped;
--   * each byte that does not start a valid UTF-8 sequence (surrogates and
--     overlongs included, as Lua 5.4's strict utf8 reads them) becomes
--     U+FFFD;
--   * the text is capped at @p max bytes (default M.TEXT_MAX), cut on a code
--     point, with "..." appended when @p ellipsis is true and it was cut.
-- The JS twin is hull:_text's storable, applying the same rules to code
-- units (a lone surrogate becomes U+FFFD). A new stdlib write of app,
-- request or remote text goes through this (CLAUDE.md).
M.TEXT_MAX = 4096
M.UNCONVERTIBLE = "(a value that could not be converted to text)"

local function cut_utf8(s, max)
    if #s <= max then return s end
    local e = max
    while e > 0 and e > max - 4 and ((s:byte(e + 1) or 0) & 0xC0) == 0x80 do e = e - 1 end
    return s:sub(1, e)
end

function M.storable(v, max, ellipsis, unconvertible)
    max = max or M.TEXT_MAX
    local s
    if type(v) == "string" then
        s = v
    elseif v == nil or type(v) == "number" or type(v) == "boolean" then
        s = tostring(v)
    else
        local ok, r = pcall(tostring, v)
        s = (ok and type(r) == "string") and r or (unconvertible or M.UNCONVERTIBLE)
    end
    local cut = false
    -- Each kept byte stays at most 3 bytes (U+FFFD for one bad byte), and a
    -- huge run of NULs keeps nothing, so 4 x max input bytes bound the scan.
    if #s > 4 * max then s = s:sub(1, 4 * max); cut = true end
    if s:find("\0", 1, true) then s = s:gsub("\0", "") end
    if not utf8.len(s) then
        local out, i, n = {}, 1, #s
        while i <= n do
            local len, bad = utf8.len(s, i)
            if len then out[#out + 1] = s:sub(i); break end
            out[#out + 1] = s:sub(i, bad - 1)
            out[#out + 1] = "\239\191\189"   -- U+FFFD
            i = bad + 1
        end
        s = table.concat(out)
    end
    if #s > max then s = cut_utf8(s, max); cut = true end
    if cut and ellipsis then s = s .. "..." end
    return s
end

return M
