--
-- hull.project.analyze — the host-owned, frontend-neutral project analyzer.
--
-- The SINGLE canonical implementation of project source discovery (design:
-- docs/project_discovery_design.md D1). Statically inspects an app's source tree WITHOUT
-- executing application code, selects a language frontend per file via the registry, and
-- returns a normalized ProjectDiscovery. Official consumers (hull agent inspect, hull
-- dev, a future build lowering step) call M.analyze; none re-scan or parse source itself.
--
-- Reuses the shared hardened walker (hull.source.discover) -- no second recursive scan.
-- Pure Lua over the tool VM; never raises, never prints (returns a data model).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local discover = require("hull.source.discover")
local registry = require("hull.project.registry")
local model    = require("hull.project.model")

local M = { SCHEMA_VERSION = model.SCHEMA_VERSION }


-- Project-relative path: strip the CANONICAL root prefix so `path` + IDs are stable +
-- portable. Handles "/" (prefix is just "/") and "." (already relative).
local function rel_path(root, path)
    if root == "." or root == "" then return path end
    if root == "/" then return (path:sub(1, 1) == "/") and path:sub(2) or path end
    local prefix = root .. "/"
    if path:sub(1, #prefix) == prefix then return path:sub(#prefix + 1) end
    return path
end

local function any_error(diags)
    for _, d in ipairs(diags or {}) do if d.severity == "error" then return true end end
    return false
end

local function stamp_path(diags, rel)
    for _, d in ipairs(diags or {}) do d.path = d.path or rel end
    return diags
end

-- Build per-source declaration facts via the frontend CONTRACT (never the AST). Each
-- decl is registered in the GENERATION handle table (`handles`) with a generation-unique
-- integer key retaining { frontend, unit, declaration } for a future lowerer to resolve
-- through the frontend boundary (M.resolve_handle). Handles are generation-internal and
-- excluded from the serialized/public projection.
local function collect_decls(fe, unit, language, rel, handles)
    local out = {}
    for _, d in ipairs(fe.declarations(unit)) do
        handles.n = handles.n + 1
        handles.map[handles.n] = { frontend = fe, unit = unit, declaration = d }
        out[#out + 1] = {
            language    = language,
            path        = rel,
            kind        = fe.decl_kind(d),
            name        = fe.decl_name(d),
            range       = fe.decl_range(d),
            group_range = fe.decl_group_range(d),
            is_method   = fe.decl_is_method(d),
            handle      = handles.n,          -- generation-unique (not the per-file index)
            annotations = fe.decl_annotations(d),
        }
    end
    return out
end

-- The UNPROTECTED analysis: root validation/canonicalization + discovery + per-file
-- frontend dispatch + model assembly. May raise on a frontend/adapter/model defect; the
-- public M.analyze wraps this in a protected boundary (D1: "never raises").
local function analyze_unprotected(root_in, opts)
    local root_disp = tostring(root_in or ".")
    local gen        = opts.generation or 0
    local source_kind = opts.source_kind or "standalone"

    -- Root validation + canonicalization (symlinks resolved). A missing / non-directory
    -- root is a project.discovery_failed generation (invalid AND incomplete), never a
    -- valid, complete, empty project. Containment + relative paths use the CANONICAL root.
    local canon, reason = tool.realpath(discover.normalize(root_disp))
    if not canon then
        local d = model.build(root_disp, gen, {}, { { severity = "error",
            code = "project.discovery_failed",
            message = "cannot resolve project root '" .. root_disp .. "' (" .. tostring(reason) .. ")",
            path = root_disp } }, source_kind)
        d.complete = false
        return d
    end
    if tool.path_kind(canon) ~= "dir" then
        local d = model.build(canon, gen, {}, { { severity = "error",
            code = "project.discovery_failed",
            message = "project root is not a directory: " .. canon, path = canon } }, source_kind)
        d.complete = false
        return d
    end
    local root = canon

    local handles = { n = 0, map = {} }
    local per_source, op_diags = {}, {}
    local files, derr = discover.discover(root, { ext = registry.known_exts(), extra_exclude = { "static" } })
    if derr then
        -- Operational discovery failure: an invalid generation, never an empty clean scan.
        op_diags[#op_diags + 1] = { severity = "error", code = "project.discovery_failed",
                                    message = "source discovery failed: " .. tostring(derr), path = root }
    else
        for _, path in ipairs(files) do
            local ext = path:match("%.([%w]+)$")
            local row = ext and registry.for_ext(ext)
            local rel = rel_path(root, path)
            if row and row.analyzable then
                local fe = registry.load(row)
                local src = tool.read_file(path)
                if not src then
                    per_source[#per_source + 1] = { path = rel, language = row.language, role = "app",
                        status = "error", declarations = {},
                        diagnostics = { { severity = "error", code = "project.unreadable",
                                          message = "cannot read source file", path = rel } } }
                else
                    local unit, diags = fe.parse(src, rel)
                    stamp_path(diags, rel)
                    if unit == nil then                          -- frontend internal / API failure
                        per_source[#per_source + 1] = { path = rel, language = row.language, role = "app",
                            status = "error", declarations = {}, diagnostics = diags }
                    else
                        per_source[#per_source + 1] = { path = rel, language = row.language, role = "app",
                            status = any_error(diags) and "error" or "analyzed",
                            declarations = collect_decls(fe, unit, row.language, rel, handles),
                            diagnostics = diags }
                    end
                end
            elseif row then
                -- Known but NON-analyzable language (JavaScript): honest unsupported app
                -- source -> the generation is complete=false (D11). Never parsed as Lua.
                per_source[#per_source + 1] = { path = rel, language = row.language, role = "app",
                    status = "unsupported",
                    diagnostics = { { severity = "warning", code = "project.frontend.unsupported",
                        message = "no analyzable frontend for language '" .. row.language .. "'",
                        path = rel, language = row.language } } }
            end
            -- An extension not in the registry is not project source; the scan never
            -- returns it (discover restricts to known_exts), so there is no else branch.
        end
    end

    local disc = model.build(root, gen, per_source, op_diags, source_kind)
    disc._handles = handles.map            -- generation-internal; NOT serialized (D6)
    return disc
end

-- Emergency INVALID discovery built as a LITERAL, with no call into model.build /
-- registry (which is where the failure being recovered from may itself live). Used only
-- on the protected-boundary failure path, so recovery can never re-raise the same defect.
local function minimal_invalid(root, gen, source_kind, code, message)
    return {
        schema_version = model.SCHEMA_VERSION,   -- a constant, not a call
        generation     = gen,
        source         = source_kind,
        project_root   = root,
        valid          = false,
        complete       = false,
        sources        = {},
        declarations   = {},
        diagnostics    = { { severity = "error", code = code, message = message, path = root } },
        frontends      = {},                     -- literal: no registry call on the failure path
        indexes        = { by_annotation = {}, by_source = {}, by_language = {},
                           by_id = {}, annotated = {} },
        summary        = { sources_total = 0, sources_analyzed = 0, sources_unsupported = 0,
                           declarations_total = 0, declarations_annotated = 0, by_language = {} },
        _by_source     = {},
        _handles       = {},
    }
end

-- analyze(root, opts?) -> ProjectDiscovery. PROTECTED public boundary: any internal
-- frontend/adapter/model failure is converted into an INVALID discovery with a structured
-- project.internal diagnostic (never a raised error, never a clean generation).
--   opts.generation  : the dev generation counter (default 0 = standalone).
--   opts.source_kind : "standalone" (default) | "dev" provenance marker.
function M.analyze(root, opts)
    -- Normalize + validate opts ONCE, up front, so neither the normal nor the recovery
    -- path ever dereferences a non-table / wrong-typed opts.
    if type(opts) ~= "table" then opts = {} end
    local gen         = (type(opts.generation) == "number") and opts.generation or 0
    local source_kind = (type(opts.source_kind) == "string") and opts.source_kind or "standalone"
    local root_disp   = tostring(root or ".")

    local ok, result = pcall(analyze_unprotected, root, { generation = gen, source_kind = source_kind })
    if ok then return result end
    -- Recovery uses the literal constructor -- if model.build was the defect, calling it
    -- again here would re-raise the same error OUTSIDE the pcall.
    return minimal_invalid(root_disp, gen, source_kind, "project.internal",
        "internal analyzer failure: " .. tostring(result))
end

-- Controlled generation-internal handle lookup: resolve an opaque declaration handle to
-- { frontend, unit, declaration } so a future lowerer can invoke frontend-specific
-- semantics (e.g. frontend.scope(unit)) THROUGH the adapter boundary. Handles are unique
-- within one generation and are not stable across generations/processes.
function M.resolve_handle(disc, handle)
    return disc and disc._handles and disc._handles[handle] or nil
end

return M
