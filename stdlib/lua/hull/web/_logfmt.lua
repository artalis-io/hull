--- Internal logfmt value formatting shared across the logging stdlib.
--
-- @module hull.web._logfmt
-- @license AGPL-3.0-or-later
--
-- Contributor-only (the `_` prefix): required by `hull.web.middleware.logger`
-- and `hull.logx`, never declared by apps. One place for the escape + quote
-- rules so the two logfmt producers can't drift (they had: logx escaped only
-- `"`, the logger middleware escaped `\ \r \n "`). See docs/stdlib_style.md
-- section 4.

local M = {}

-- Control bytes (< 0x20, 0x7f) other than the three with a short escape go
-- out as \xHH, so no raw control byte (an ESC sequence for a terminal, a NUL
-- or a vertical tab for a log shipper) reaches the line.
local CTRL_ESC = { ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }
local function ctrl(c)
    return CTRL_ESC[c] or string.format("\\x%02x", c:byte())
end

-- The Unicode line breaks a log viewer or a JSON-lines shipper may split on
-- besides \n and \r (audit 11): the C1 controls U+0080..U+009F (U+0085 is
-- NEL) and U+2028 / U+2029, as UTF-8. They go out as \uXXXX - the JS twin
-- escapes the same code points the same way, so the output is identical.
local function c1(b)
    return string.format("\\u%04x", b:byte())
end
local function unicode_breaks(s)
    s = s:gsub("\194([\128-\159])", c1)
    s = s:gsub("\226\128\168", "\\u2028")
    s = s:gsub("\226\128\169", "\\u2029")
    return s
end
local function has_unicode_break(s)
    return s:find("\194[\128-\159]") ~= nil or s:find("\226\128[\168\169]") ~= nil
end

--- Escape a value for safe logfmt output (log-injection defense): a raw
-- newline could otherwise forge a second log line. Escapes backslash and
-- double-quote, `\n` `\r` `\t`, every other byte < 0x20 or 0x7f as `\xHH`, and
-- the C1 controls and U+2028 / U+2029 as `\uXXXX`.
-- @tparam any v
-- @treturn string
function M.sanitize(v)
    local s = tostring(v)
    s = s:gsub("\\", "\\\\")
    s = s:gsub('"', '\\"')
    s = s:gsub("[%z\1-\31\127]", ctrl)
    return unicode_breaks(s)
end

--- Format a free-text log message as the line's leading `msg="..."` field,
-- always quoted and escaped as a value (audit 11). It was written bare, with
-- only control bytes escaped, so a message carrying ` user=admin` forged a
-- field for any logfmt reader.
-- @tparam any v
-- @treturn string  e.g. `msg="handled"`
function M.message(v)
    return 'msg="' .. M.sanitize(v) .. '"'
end

--- Make a logfmt key: every byte outside `[A-Za-z0-9_.-]` becomes `_` (an
-- empty key is `_`), so a key cannot carry a space, `=`, a quote or a newline
-- and split or forge a pair.
-- @tparam any k
-- @treturn string
function M.key(k)
    local s = tostring(k):gsub("[^%w_%.%-]", "_")
    if s == "" then return "_" end
    return s
end

--- Format one `key=value` logfmt pair: the key through `key`, the value
-- through `sanitize`, quoted when the RAW value contains a space, `=`, `"` or
-- any control byte (matches the format any logfmt reader expects). The quote
-- test is on the raw value so an escaped newline still triggers quoting.
-- @tparam string k
-- @tparam any v
-- @treturn string  e.g. `k=v` or `k="v with spaces"`.
function M.pair(k, v)
    local raw = tostring(v)
    local s = M.sanitize(raw)
    local key = M.key(k)
    if raw:find('[ ="%z\1-\31\127]') or has_unicode_break(raw) then
        return key .. '="' .. s .. '"'
    end
    return key .. "=" .. s
end

return M
