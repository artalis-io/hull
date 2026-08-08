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

--- Escape a value for safe logfmt output (log-injection defense): a raw
-- newline could otherwise forge a second log line. Escapes backslash, CR, LF,
-- and double-quote.
-- @tparam any v
-- @treturn string
function M.sanitize(v)
    local s = tostring(v)
    s = s:gsub("\\", "\\\\")
    s = s:gsub("\n", "\\n")
    s = s:gsub("\r", "\\r")
    s = s:gsub('"', '\\"')
    return s
end

--- Format one `key=value` logfmt pair, quoting the value when the RAW value
-- contains a space, `=`, `"`, or a CR/LF (matches the format any logfmt reader
-- expects). The quote test is on the raw value so an escaped newline still
-- triggers quoting.
-- @tparam string k
-- @tparam any v
-- @treturn string  e.g. `k=v` or `k="v with spaces"`.
function M.pair(k, v)
    local raw = tostring(v)
    local s = M.sanitize(raw)
    if raw:find('[ =\n\r"]') then
        return k .. '="' .. s .. '"'
    end
    return k .. "=" .. s
end

return M
