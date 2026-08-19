--
-- hull.project.frontend_javascript - the Lua PROXY for the JavaScript frontend.
--
-- Implements the frontend CONTRACT (the same accessor surface as hull.project.frontend_lua) but
-- delegates every operation to the C bridge (tool.frontend_*), which runs the bundled JS adapter
-- in a QuickJS tooling session. This module NEVER parses JavaScript or touches QuickJS.
--
-- The analyzer's _handles payload stays frontend-neutral: { frontend = <this>, unit, declaration }.
-- The proxy supplies JS-shaped `unit` / `declaration` objects that carry their bridge-private
-- integers INTERNALLY (_session/_unit_id/_decl_id), so fe.declaration_semantics(declaration) and
-- fe.scope(unit) work verbatim and the analyzer never sees a token.
--
-- Design: docs/js_frontend_slice6_dispatcher.md. Never raises: every entry returns data or a
-- Diagnostic-shaped error.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

local M = {}

M.capabilities = { "declarations", "annotations", "source_ranges", "scope", "semantics" }

local function diag(code, message)
    return { severity = "error", code = code, message = message }
end

-- Decode a bridge JSON string, or return (nil, Diagnostic) on a malformed/absent payload.
local function decode(raw, what)
    if type(raw) ~= "string" then return nil, diag("javascript.internal", (what or "frontend") .. ": no bridge result") end
    local ok, data = pcall(json.decode, raw)
    if not ok or type(data) ~= "table" then
        return nil, diag("javascript.internal", (what or "frontend") .. ": malformed bridge JSON")
    end
    return data, nil
end

-- analyze_source(session, path, src) -> (unit, decls, diags). Calls the C bridge once, decodes the
-- facts, and builds the JS `unit` object + one JS `declaration` object per fact. `diags` are the
-- normalized per-source diagnostics; on a bridge/transport failure a single internal diagnostic.
function M.analyze_source(session, path, src)
    local raw = tool.frontend_analyze("javascript", session, path, src)
    local facts, derr = decode(raw, "analyze")
    if not facts then
        return nil, {}, { derr }
    end
    local decls = {}
    for _, f in ipairs(facts.declarations or {}) do
        decls[#decls + 1] = {
            _session = session,
            _decl_id = f.decl_id,
            _name = f.name, _kind = f.kind, _range = f.range, _group = f.group_range,
            _is_method = f.is_method == true, _anns = f.annotations or {},
        }
    end
    local unit = { _session = session, _unit_id = facts.unit_id, _declarations = decls }
    return unit, decls, facts.diagnostics or {}
end

-- Accessor contract (mirrors frontend_lua), reading the normalized fields off the JS declaration.
function M.declarations(unit)   return (unit and unit._declarations) or {} end
function M.decl_name(d)         return d._name end
function M.decl_kind(d)         return d._kind end
function M.decl_range(d)        return d._range end
function M.decl_group_range(d)  return d._group end
function M.decl_annotations(d)  return d._anns end
function M.decl_is_method(d)    return d._is_method == true end

-- declaration_semantics(declaration) -> (record, nil) | (nil, Diagnostic). Reached ONLY through
-- the analyzer's lifecycle-gated analyze.declaration_semantics; extracts the private integers.
function M.declaration_semantics(d)
    if type(d) ~= "table" or type(d._session) ~= "number" or type(d._decl_id) ~= "number" then
        return nil, diag("javascript.internal", "declaration_semantics: invalid JS declaration handle")
    end
    local raw = tool.frontend_declaration_semantics("javascript", d._session, d._decl_id)
    local data, err = decode(raw, "declaration_semantics")
    if not data then return nil, err end
    if data.error then return nil, data.error end
    return data, nil
end

-- scope(unit) -> (model, nil) | (nil, Diagnostic).
function M.scope(unit)
    if type(unit) ~= "table" or type(unit._session) ~= "number" or type(unit._unit_id) ~= "number" then
        return nil, diag("javascript.internal", "scope: invalid JS unit handle")
    end
    local raw = tool.frontend_scope("javascript", unit._session, unit._unit_id)
    local data, err = decode(raw, "scope")
    if not data then return nil, err end
    if data.ok == false then return nil, data.error or diag("javascript.internal", "scope failed") end
    return data, nil
end

return M
