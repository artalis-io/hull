--
-- hull.source.lua — the PUBLIC Lua source-analysis contract.
--
--   local lua = require("hull.source.lua")
--   local unit, err = lua.parse(source, { path = "src/example.lua" })
--
-- parse() contract (locked, docs/lua_source_analysis_design.md):
--   * `source` must be a string. `opts` optional: { path?, limits? }.
--   * Ordinary syntax problems => a SourceUnit is returned with err == nil; the
--     problems are in unit.diagnostics (severity "error"). "Did it parse cleanly?"
--     is `#unit.diagnostics == 0`, NOT `err`.
--   * err ~= nil (and unit == nil) is reserved for invalid API arguments and
--     internal parser failure. err is a Diagnostic-shaped table (or a string for
--     pure API misuse). No raw Lua error() ever crosses this boundary.
--   * Never prints. Diagnostics are data.
--
-- SLICE 1 (this file): lexer + ranges + line map. `unit.ast` is nil until the
-- statement slice lands; `unit.tokens`/`unit.comments`/`unit.diagnostics` and the
-- unit:text/position/line_col helpers are live now.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lexer = require("hull.source.lexer")
local range = require("hull.source.range")
local diag = require("hull.source.diagnostic")

local M = {}

local Unit = {}
Unit.__index = Unit

-- Exact original text for a node/range (anything carrying a `.range`, or a range).
function Unit:text(x)
    local r = x.range or x
    return self.source:sub(r.start, r.stop - 1)
end

-- 1-based (line, col) for a byte offset; col is a byte column within the line.
function Unit:position(off)
    return range.position(self._linestarts, off)
end

-- (start_line, start_col, end_line, end_col) for a range. end_col is resolved at
-- the exclusive `stop` (one past the last byte).
function Unit:line_col(r)
    r = r.range or r
    local sl, sc = range.position(self._linestarts, r.start)
    local el, ec = range.position(self._linestarts, r.stop)
    return sl, sc, el, ec
end

function M.parse(source, opts)
    if type(source) ~= "string" then
        return nil, "hull.source.lua: source must be a string, got " .. type(source)
    end
    opts = opts or {}

    -- Defense in depth: the lexer is written not to raise, but a bug must still
    -- surface as `err`, never as a raw error crossing parse().
    local ok, res = pcall(lexer.tokenize, source, opts)
    if not ok then
        return nil, diag.error("lua.internal",
            "internal lexer failure: " .. tostring(res), opts.path, nil)
    end

    local unit = setmetatable({
        path = opts.path,
        language = "lua",
        source = source,
        ast = nil,                       -- slice 3
        tokens = res.tokens,             -- slice-1 surface; the parser consumes these
        comments = res.comments,
        diagnostics = res.diagnostics,
        _linestarts = range.linemap(source),
    }, Unit)

    return unit, nil
end

return M
