--
-- hull.source.diagnostic - language-neutral structured diagnostic.
--
-- THE shared diagnostic shape for the whole source-analysis layer (Lua now, JS
-- later, and eventually `hull analyze`). Never printed - diagnostics are data,
-- returned in unit.diagnostics. A parse() API-misuse / internal-failure error is
-- also diagnostic-shaped (returned as the second value with unit == nil).
--
--   { severity = "error" | "warning",
--     code     = "lua.syntax" | "lua.unsupported" | "lua.limit.<which>" | "lua.internal",
--     message  = string,
--     path     = string | nil,
--     range    = { start, stop } | nil,      -- half-open byte range (see range.lua)
--     related  = { { message, path?, range? }, ... } | nil }
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

function M.new(severity, code, message, path, range, related)
    return {
        severity = severity,
        code = code,
        message = message,
        path = path,
        range = range,
        related = related,
    }
end

function M.error(code, message, path, range)
    return M.new("error", code, message, path, range)
end

function M.warning(code, message, path, range)
    return M.new("warning", code, message, path, range)
end

return M
