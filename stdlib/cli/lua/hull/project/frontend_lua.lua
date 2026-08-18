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
local scope = require("hull.source.scope")

local M = {
    language     = "lua",
    extensions   = { "lua" },
    -- Only capabilities Lua actually SHIPS (D3), and each is callable THROUGH this
    -- contract (declarations/annotations/source_ranges via decl_*; scope via M.scope;
    -- semantics via M.declaration_semantics). Nothing unimplemented is advertised.
    capabilities = { "declarations", "annotations", "source_ranges", "scope", "semantics" },
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
-- its own name range + the node's annotations. The GENERATION-unique handle is assigned
-- by the analyzer (which owns the generation), not here -- this returns opaque decl
-- objects the analyzer retains behind its handle table.
function M.declarations(unit)
    local out = {}
    lua.walk(unit.ast, function(n)
        if n.kind == "local_declaration" then
            local anns, grp = norm_annotations(unit, n), mkrange(unit, n.range)
            for idx, nm in ipairs(n.names or {}) do
                -- `_node` (the declaration AST node) + `_name_index` (this name's position)
                -- are PRIVATE frontend state -- read only by M.declaration_semantics, never
                -- serialized (the neutral model builds facts from the accessors, not from
                -- this object; the handle table that holds it is not on the wire).
                out[#out + 1] = { _kind = "local", _name = nm.name, _range = mkrange(unit, nm.range),
                                  _group = grp, _is_method = false, _anns = anns,
                                  _node = n, _name_index = idx }
            end
        elseif n.kind == "function_declaration" then
            local name = flatten_fnname(n.name, n.is_method)
            if name then
                out[#out + 1] = {
                    _kind = n.is_local and "local_function" or "function",
                    _name = name,
                    _range = mkrange(unit, (n.name and n.name.range) or n.range),
                    _group = mkrange(unit, n.range),
                    _is_method = n.is_method == true,
                    _anns = norm_annotations(unit, n),
                    _node = n,                       -- private frontend state (see above)
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

local function sem_err(msg)
    return { severity = "error", code = "lua.internal", message = "declaration_semantics: " .. msg }
end

-- declaration_semantics(decl) -> (sem, err): recover the Lua-frontend SOURCE SEMANTICS of a
-- discovered declaration for a future Lua-specific lowering step (Query / Compute). Reached
-- ONLY via M.resolve_handle -> {frontend, unit, declaration}; the neutral ProjectDiscovery
-- never carries this. `sem` is a small frontend-SPECIFIC record over the retained AST node
-- (opaque to the neutral model; a Lua lowerer inspects it, the wire never sees it):
--   * a `local` decl -> { form="value", name_index, values, positional_value }. `values` is
--     the COMPLETE right-hand-side expression list; `name_index` is this name's position.
--     `positional_value` is `values[name_index]` -- a CONVENIENCE for the common 1:1 case,
--     which is nil for a name that has no positional initializer. Lua multiple assignment is
--     NOT positional when the final RHS expression can return multiple values
--     (`local a, b = f()` binds both a AND b from f()), so the full `values` + `name_index`
--     preserve the real semantics -- a lowerer must not read `positional_value` as "the only
--     source of this name". Expression nodes carry an exact `.kind` + byte `.range` from the
--     parser (no ranges synthesized).
--   * a `local_function` / `function` decl -> { form="function", is_method, is_vararg,
--     params, body } where `params`/`body` are the parser's exact param/statement subtrees.
-- `err` (a Diagnostic-shaped table) is returned for any impossible/corrupt frontend state
-- (missing `_node`, a node whose `.kind` does not match the declaration kind, a bad
-- `_name_index`, a malformed values/params/body, or an unsupported kind) -- never for an
-- ordinary "no initializer", and never for an unsupported LOWERING construct (that belongs
-- to the future domain lowerer, not to discovery).
function M.declaration_semantics(d)
    if type(d) ~= "table" or type(d._node) ~= "table" then
        return nil, sem_err("declaration is missing its frontend AST node")
    end
    local n, kind = d._node, d._kind
    if kind == "local" then
        if n.kind ~= "local_declaration" then
            return nil, sem_err("node/kind mismatch: expected local_declaration, got '" .. tostring(n.kind) .. "'")
        end
        if math.type(d._name_index) ~= "integer" or d._name_index < 1 then
            return nil, sem_err("missing or invalid name index")
        end
        if type(n.values) ~= "table" then
            return nil, sem_err("malformed local declaration (values not a list)")
        end
        return { form = "value", name_index = d._name_index, values = n.values,
                 positional_value = n.values[d._name_index] }
    elseif kind == "local_function" or kind == "function" then
        if n.kind ~= "function_declaration" then
            return nil, sem_err("node/kind mismatch: expected function_declaration, got '" .. tostring(n.kind) .. "'")
        end
        if type(n.params) ~= "table" or type(n.body) ~= "table" then
            return nil, sem_err("malformed function declaration (params/body not lists)")
        end
        return { form = "function", is_method = d._is_method == true, is_vararg = n.is_vararg == true,
                 params = n.params, body = n.body }
    end
    return nil, sem_err("unsupported declaration kind '" .. tostring(kind) .. "'")
end

-- scope(result) -> (scope, err): the advertised "scope" capability, callable THROUGH the
-- adapter (D-scope fix). Protected wrapper around hull.source.scope.resolve so a consumer
-- (e.g. a future lowerer, via M.resolve_handle -> {frontend, unit, ...}) never bypasses
-- the frontend boundary or sees a raised error. `err` is a Diagnostic-shaped table.
function M.scope(unit)
    local ok, sc, err = pcall(scope.resolve, unit)
    if not ok then
        return nil, { severity = "error", code = "lua.internal",
                      message = "scope resolution failed: " .. tostring(sc) }
    end
    return sc, err
end

return M
