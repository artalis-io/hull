--
-- hull.project.registry - the ONE canonical extension -> frontend mapping.
--
-- Design: docs/project_discovery_design.md D3/D11. Adding a frontend is one row here;
-- hull dev / hull agent / build consumers ask the registry and never change. Lua is the
-- only ANALYZABLE frontend today. JavaScript is a KNOWN language, reserved
-- architecturally, analyzable = false (no parser, no regex scanner, no parity pretence):
-- a `.js` application source is honestly reported unsupported, never parsed as Lua.
--
-- Pure Lua; the Lua frontend module is loaded lazily (require) only when a `.lua` source
-- is actually analyzed.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- Extension (no dot) -> registry row. `engine` selects HOW analyze_one reaches the frontend
-- ("lua" = in-process; "javascript" = the C bridge). A JS row's analyzability is CONDITIONAL on
-- the tooling engine being compiled in (see `for_ext`): the QuickJS frontend session is not linked
-- in a lua-only hull, so JavaScript is analyzable only when tool.frontend_available("javascript").
local FRONTENDS = {
    lua = { language = "lua",        engine = "lua",        frontend_module = "hull.project.frontend_lua",
            extra_capabilities = nil },
    js  = { language = "javascript", engine = "javascript", frontend_module = "hull.project.frontend_javascript",
            extra_capabilities = {} },
    mjs = { language = "javascript", engine = "javascript", frontend_module = "hull.project.frontend_javascript",
            extra_capabilities = {} },
    cjs = { language = "javascript", engine = "javascript", frontend_module = "hull.project.frontend_javascript",
            extra_capabilities = {} },
}

-- True iff the tooling engine for a row is compiled into this hull. Lua is always in; JavaScript
-- needs the QuickJS frontend session (tool.frontend_available). Queried each time (the binding is
-- a cheap constant in production; not memoized, so a test that swaps the tool stub sees the change).
local function engine_available(engine)
    if engine == "lua" then return true end
    if engine == nil then return false end
    local ok, v = pcall(function() return tool.frontend_available(engine) end)
    return ok and v == true
end

-- Every known-language extension (sorted). These are the extensions the project scan
-- collects as candidate APPLICATION source; analyzable ones get a frontend, the rest are
-- reported unsupported (D11). An extension NOT here is not project source at all.
function M.known_exts()
    local out = {}
    for ext in pairs(FRONTENDS) do out[#out + 1] = ext end
    table.sort(out)
    return out
end

-- The registry row for an extension (no dot), or nil if unknown. Returns a COPY whose
-- `analyzable` is computed at query time from engine availability (so a JS-less hull reports
-- JavaScript known-but-unanalyzable, exactly as before this frontend shipped).
function M.for_ext(ext)
    local row = FRONTENDS[ext]
    if not row then return nil end
    local r = {}
    for k, v in pairs(row) do r[k] = v end
    r.analyzable = engine_available(row.engine)
    return r
end

-- Load (require) the frontend module for an analyzable row. Returns the frontend table,
-- or nil for a non-analyzable / unknown row.
function M.load(row)
    if not (row and row.analyzable and row.frontend_module) then return nil end
    return require(row.frontend_module)
end

-- Frontend summary for the ProjectDiscovery model: one entry per distinct LANGUAGE with
-- its extensions grouped, its capabilities (from the loaded frontend, or the row's
-- reserved set), and analyzable flag. Deterministic order.
function M.frontends()
    local by_lang = {}
    for _, ext in ipairs(M.known_exts()) do
        local row = M.for_ext(ext)                 -- computed analyzable (engine availability)
        local e = by_lang[row.language]
        if not e then
            local caps = {}
            local fe = M.load(row)
            if fe and fe.capabilities then
                for _, c in ipairs(fe.capabilities) do caps[#caps + 1] = c end
            elseif row.extra_capabilities then
                for _, c in ipairs(row.extra_capabilities) do caps[#caps + 1] = c end
            end
            e = { language = row.language, extensions = {}, capabilities = caps,
                  analyzable = row.analyzable == true }
            by_lang[row.language] = e
        end
        e.extensions[#e.extensions + 1] = ext
    end
    local out = {}
    for _, e in pairs(by_lang) do table.sort(e.extensions); out[#out + 1] = e end
    table.sort(out, function(a, b) return a.language < b.language end)
    return out
end

return M
