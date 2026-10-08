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

--- Escape a value for safe logfmt output (log-injection defense): a raw
-- newline could otherwise forge a second log line. Escapes backslash and
-- double-quote, `\n` `\r` `\t`, and every other byte < 0x20 or 0x7f as `\xHH`.
-- @tparam any v
-- @treturn string
function M.sanitize(v)
    local s = tostring(v)
    s = s:gsub("\\", "\\\\")
    s = s:gsub('"', '\\"')
    s = s:gsub("[%z\1-\31\127]", ctrl)
    return s
end

--- Escape the control bytes of a free-text log message (the part before the
-- fields): `\n` `\r` `\t` and `\xHH`, as in a value. Backslash and quote stay
-- as written - a message is not a logfmt value.
-- @tparam any v
-- @treturn string
function M.message(v)
    return (tostring(v):gsub("[%z\1-\31\127]", ctrl))
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
    if raw:find('[ ="%z\1-\31\127]') then
        return key .. '="' .. s .. '"'
    end
    return key .. "=" .. s
end

return M
