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
-- Offsets outside [1, #source + 1] are CLAMPED to that valid range (a byte past
-- end-of-source resolves to #source+1, the position after the last byte) so a
-- caller passing a stray offset gets a meaningful boundary position, never a
-- meaningless column.
function Unit:position(off)
    local maxoff = #self.source + 1
    if type(off) ~= "number" then off = 1 end
    if off < 1 then off = 1 elseif off > maxoff then off = maxoff end
    return range.position(self._linestarts, off)
end

-- (start_line, start_col, end_line, end_col) for a range. end_col is resolved at
-- the exclusive `stop` (one past the last byte). Routes through :position so the
-- same clamping applies.
function Unit:line_col(r)
    r = r.range or r
    local sl, sc = self:position(r.start)
    local el, ec = self:position(r.stop)
    return sl, sc, el, ec
end

-- Limit keys validated at the boundary (each, if present, must be a non-negative
-- integer). Invalid opts/limits are API MISUSE -> a clear (nil, err) BEFORE the
-- lexer runs, not a lua.internal failure surfacing through the pcall.
local LIMIT_KEYS = { "max_bytes", "max_tokens", "max_comments", "max_diagnostics", "max_depth" }

function M.parse(source, opts)
    if type(source) ~= "string" then
        return nil, "hull.source.lua: source must be a string, got " .. type(source)
    end
    if opts ~= nil and type(opts) ~= "table" then
        return nil, "hull.source.lua: opts must be a table or nil, got " .. type(opts)
    end
    opts = opts or {}
    if opts.path ~= nil and type(opts.path) ~= "string" then
        return nil, "hull.source.lua: opts.path must be a string or nil, got " .. type(opts.path)
    end
    if opts.limits ~= nil then
        if type(opts.limits) ~= "table" then
            return nil, "hull.source.lua: opts.limits must be a table or nil, got " .. type(opts.limits)
        end
        for _, k in ipairs(LIMIT_KEYS) do
            local v = opts.limits[k]
            if v ~= nil and (type(v) ~= "number" or math.type(v) ~= "integer" or v < 0) then
                return nil, "hull.source.lua: opts.limits." .. k .. " must be a non-negative integer"
            end
        end
    end

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
