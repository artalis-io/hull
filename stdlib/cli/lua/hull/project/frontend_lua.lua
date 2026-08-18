--
-- hull.project.frontend_lua — the production Lua FRONTEND ADAPTER.
--
-- Adapts the shipped hull.source.lua AST into the frontend contract the project
-- analyzer consumes (design: docs/project_discovery_design.md D3/D4). This is the ONLY
-- project module that knows Lua node-field layouts; the analyzer + model talk to the
-- contract accessors below, never to the AST.
--
-- Contract (a plain table of functions, Hull's registry convention -- not an OO class):
--   language, extensions, capabilities, analyzable
--   parse(source, path) -> (result, diagnostics[])   result is opaque (the SourceUnit)
--   declarations(result) -> decl[]                    each decl opaque to the caller
--   decl_name(d) / decl_kind(d) / decl_range(d) / decl_group_range(d)
--   decl_annotations(d) -> normalized annotation[]    { name, args?, value?, raw, range }
--   decl_is_method(d) / decl_handle(d)                handle = generation-local opaque int
--
-- Pure Lua; never raises, never prints (parse() surfaces problems as diagnostics data).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")

local M = {
    language     = "lua",
    extensions   = { "lua" },
    -- Only capabilities Lua actually SHIPS (D3). Not bindings/semantic_analysis/lowering.
    capabilities = { "declarations", "annotations", "source_ranges", "scope" },
    analyzable   = true,
}

-- ── AST-aware internals (the containment boundary: nothing below escapes this file) ──

-- A range as a plain {start, stop, line, col} table (1-based line/col at `start`).
local function mkrange(unit, r)
    if type(r) ~= "table" or type(r.start) ~= "number" then return nil end
    local line, col = unit:line_col(r)
    return { start = r.start, stop = r.stop, line = line, col = col }
end

-- Normalize a declaration node's attached `---@` annotations into plain records. The
-- annotation layer already ran during parse (lua.parse -> annotations.attach), so the
-- node carries annotation_list. `value` is the trailing free text after the name/args.
local function norm_annotations(unit, node)
    local out = {}
    for _, a in ipairs(node.annotation_list or {}) do
        out[#out + 1] = {
            name  = a.name,
            args  = a.args,          -- balanced (...) group text, or nil
            value = a.text,          -- trailing text after name/(...), trimmed, or nil
            raw   = a.raw,
            range = mkrange(unit, a.range),
        }
    end
    return out
end

-- Flatten a function_declaration name (a `name` node or a `field` chain) to a dotted /
-- method string: `a.b.c` for `function a.b.c()`, `a:m` for `function a:m()`, `f` for a
-- plain / local function. Consumers never see the chain (D4).
local function flatten_fnname(namenode, is_method)
    local parts, n = {}, namenode
    while type(n) == "table" do
        if n.kind == "name" then parts[#parts + 1] = n.name; break
        elseif n.kind == "field" then parts[#parts + 1] = n.name; n = n.obj
        else break end
    end
    local rev = {}
    for i = #parts, 1, -1 do rev[#rev + 1] = parts[i] end
    if #rev == 0 then return nil end
    if is_method and #rev >= 2 then
        return table.concat(rev, ".", 1, #rev - 1) .. ":" .. rev[#rev]
    end
    return table.concat(rev, ".")
end

-- ── the contract ──

-- parse(source, path) -> (result, diagnostics[]). result is the opaque SourceUnit;
-- diagnostics are normalized { severity, code, message, range? }. On API-misuse /
-- internal failure (unit == nil) returns (nil, { one internal diagnostic }).
function M.parse(source, path)
    local unit, err = lua.parse(source, { path = path })
    if unit == nil then
        local d
        if type(err) == "table" then
            d = { severity = err.severity or "error", code = err.code or "lua.internal",
                  message = err.message or "internal parser failure", range = err.range }
        else
            d = { severity = "error", code = "lua.internal", message = tostring(err) }
        end
        return nil, { d }
    end
    local diags = {}
    for _, d in ipairs(unit.diagnostics) do
        diags[#diags + 1] = {
            severity = d.severity or "error",
            code     = d.code or "lua.syntax",
            message  = d.message or "",
            range    = mkrange(unit, d.range),
        }
    end
    return unit, diags
end

-- declarations(result) -> decl[]. ONE decl per declared NAME (D4): a multi-name
-- `local a, b` yields two decls sharing the declaration NODE's group range; each carries
-- its own name range + the node's annotations. Handles are generation-local indices.
function M.declarations(unit)
    local out, idx = {}, 0
    lua.walk(unit.ast, function(n)
        if n.kind == "local_declaration" then
            local anns, grp = norm_annotations(unit, n), mkrange(unit, n.range)
            for _, nm in ipairs(n.names or {}) do
                idx = idx + 1
                out[#out + 1] = { _kind = "local", _name = nm.name, _range = mkrange(unit, nm.range),
                                  _group = grp, _is_method = false, _anns = anns, _index = idx }
            end
        elseif n.kind == "function_declaration" then
            local name = flatten_fnname(n.name, n.is_method)
            if name then
                idx = idx + 1
                out[#out + 1] = {
                    _kind = n.is_local and "local_function" or "function",
                    _name = name,
                    _range = mkrange(unit, (n.name and n.name.range) or n.range),
                    _group = mkrange(unit, n.range),
                    _is_method = n.is_method == true,
                    _anns = norm_annotations(unit, n),
                    _index = idx,
                }
            end
        end
    end)
    return out
end

function M.decl_name(d)        return d._name end
function M.decl_kind(d)        return d._kind end
function M.decl_range(d)       return d._range end
function M.decl_group_range(d) return d._group end
function M.decl_annotations(d) return d._anns end
function M.decl_is_method(d)   return d._is_method == true end
function M.decl_handle(d)      return d._index end      -- generation-local; never serialized as-is

return M
