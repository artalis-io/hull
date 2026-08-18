--
-- hull.project.registry — the ONE canonical extension -> frontend mapping.
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

-- Extension (no dot) -> registry row. `frontend_module` present + analyzable => a real
-- frontend to require; analyzable=false => a reserved, known-but-unanalyzable language.
local FRONTENDS = {
    lua = { language = "lua",        frontend_module = "hull.project.frontend_lua",
            analyzable = true,  extra_capabilities = nil },
    js  = { language = "javascript", analyzable = false, extra_capabilities = {} },
    mjs = { language = "javascript", analyzable = false, extra_capabilities = {} },
    cjs = { language = "javascript", analyzable = false, extra_capabilities = {} },
}

-- Every known-language extension (sorted). These are the extensions the project scan
-- collects as candidate APPLICATION source; analyzable ones get a frontend, the rest are
-- reported unsupported (D11). An extension NOT here is not project source at all.
function M.known_exts()
    local out = {}
    for ext in pairs(FRONTENDS) do out[#out + 1] = ext end
    table.sort(out)
    return out
end

-- The registry row for an extension (no dot), or nil if the extension is not known.
function M.for_ext(ext) return FRONTENDS[ext] end

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
        local row = FRONTENDS[ext]
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
