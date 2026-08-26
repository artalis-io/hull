--
-- hull.project.analyze - the host-owned, frontend-neutral project analyzer.
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

local function diag(code, message, path)
    return { severity = "error", code = code, message = message, path = path }
end

-- The uniform per-file seam: dispatch on the row's engine. `ctx` carries the generation's JS
-- session token (opened lazily, latched on failure). Returns a per_source entry. Lua runs the
-- in-process frontend; JavaScript makes one C-bridge call, both producing the identical facts
-- shape and the SAME { frontend, unit, declaration } handle payload (via collect_decls).
local function analyze_one(fe, row, src, rel, handles, ctx)
    if row.engine == "javascript" then
        if ctx.js_open_failed then
            return { path = rel, language = row.language, role = "app", status = "error", declarations = {},
                     diagnostics = { diag("javascript.internal",
                         "JavaScript frontend session unavailable: " .. tostring(ctx.js_open_reason), rel) } }
        end
        if not ctx.js_token then                                   -- lazy open, once per generation
            local token, oerr = tool.frontend_open("javascript")
            if not token then
                ctx.js_open_failed = true
                ctx.js_open_reason = oerr or "frontend session open failed"
                return { path = rel, language = row.language, role = "app", status = "error", declarations = {},
                         diagnostics = { diag("javascript.internal",
                             "JavaScript frontend session unavailable: " .. tostring(ctx.js_open_reason), rel) } }
            end
            ctx.js_token = token
        end
        local unit, _decls, diags = fe.analyze_source(ctx.js_token, rel, src)
        stamp_path(diags, rel)
        if unit == nil then
            return { path = rel, language = row.language, role = "app", status = "error",
                     declarations = {}, diagnostics = diags }
        end
        return { path = rel, language = row.language, role = "app",
                 status = any_error(diags) and "error" or "analyzed",
                 declarations = collect_decls(fe, unit, row.language, rel, handles),
                 diagnostics = diags }
    end
    -- lua (in-process)
    local unit, diags = fe.parse(src, rel)
    stamp_path(diags, rel)
    if unit == nil then
        return { path = rel, language = row.language, role = "app", status = "error",
                 declarations = {}, diagnostics = diags }
    end
    return { path = rel, language = row.language, role = "app",
             status = any_error(diags) and "error" or "analyzed",
             declarations = collect_decls(fe, unit, row.language, rel, handles),
             diagnostics = diags }
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

    local retain = (opts.retain_frontend == true)     -- strict boolean; only `true` retains
    local ctx = { js_token = nil, js_open_failed = false }
    local handles = { n = 0, map = {} }

    -- The per-file loop + model.build run under a pcall so the generation's JS session is ALWAYS
    -- closed on a fault (finally-style), then the fault is re-raised for M.analyze's boundary.
    local okb, disc = pcall(function()
        local per_source, op_diags = {}, {}
        local files, derr = discover.discover(root, { ext = registry.known_exts(), extra_exclude = { "static" } })
        if derr then
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
                            diagnostics = { diag("project.unreadable", "cannot read source file", rel) } }
                    else
                        per_source[#per_source + 1] = analyze_one(fe, row, src, rel, handles, ctx)
                    end
                elseif row then
                    -- Known but NON-analyzable language (e.g. JavaScript with the engine not
                    -- compiled in): honest unsupported app source -> complete=false (D11).
                    per_source[#per_source + 1] = { path = rel, language = row.language, role = "app",
                        status = "unsupported",
                        diagnostics = { { severity = "warning", code = "project.frontend.unsupported",
                            message = "no analyzable frontend for language '" .. row.language .. "'",
                            path = rel, language = row.language } } }
                end
            end
        end
        local d = model.build(root, gen, per_source, op_diags, source_kind)
        d._handles = handles.map            -- generation-internal; NOT serialized (D6)
        return d
    end)

    if not okb then
        if ctx.js_token then pcall(function() tool.frontend_close("javascript", ctx.js_token) end) end
        error(disc, 0)                      -- re-raise; M.analyze converts to a minimal invalid
    end

    -- Attach the FRONTEND-NEUTRAL generation lease (docs 7.1). Default: close the JS session now
    -- (metadata-only discovery). Retained: keep it open until analyze.close(disc). Both gate Lua
    -- AND JavaScript semantics identically -- physical AST/session lifetime never leaks out.
    if retain then
        disc._frontend_lease = { retained = true, open = true,
                                 sessions = ctx.js_token and { javascript = ctx.js_token } or {} }
    else
        if ctx.js_token then pcall(function() tool.frontend_close("javascript", ctx.js_token) end) end
        disc._frontend_lease = { retained = false, open = false, sessions = {} }
    end
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
        _frontend_lease = { retained = false, open = false, sessions = {} },  -- not semantically live
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

    local retain = (opts.retain_frontend == true)      -- strict boolean; threaded to the analysis
    local ok, result = pcall(analyze_unprotected, root,
        { generation = gen, source_kind = source_kind, retain_frontend = retain })
    if ok then return result end
    -- Recovery uses the literal constructor -- if model.build was the defect, calling it
    -- again here would re-raise the same error OUTSIDE the pcall.
    return minimal_invalid(root_disp, gen, source_kind, "project.internal",
        "internal analyzer failure: " .. tostring(result))
end

-- INTERNAL generation-handle lookup: resolve an opaque declaration handle to
-- { frontend, unit, declaration }. This is NO LONGER the public semantic path (it bypasses the
-- lifecycle gate); the supported path is M.declaration_semantics / M.scope below. Kept for
-- lower-level use / tests. Handles are unique within one generation, not stable across
-- generations/processes.
function M.resolve_handle(disc, handle)
    return disc and disc._handles and disc._handles[handle] or nil
end

-- The frontend-neutral generation-lease gate: semantics/scope are available ONLY on a retained,
-- OPEN discovery, identically for every frontend (docs 7). Returns (true) or (nil, Diagnostic).
local function lease_check(disc)
    local lease = type(disc) == "table" and disc._frontend_lease or nil
    if type(lease) ~= "table" or lease.retained ~= true then
        return nil, diag("project.frontend_not_retained",
            "frontend semantics are available only on a discovery created with retain_frontend = true")
    end
    if lease.open ~= true then
        return nil, diag("project.frontend_closed", "the frontend generation has been closed")
    end
    return true
end

-- analyze.declaration_semantics(disc, handle) -> (record, nil) | (nil, Diagnostic). The SOLE
-- supported semantic-lowering path: validate the lease (frontend-neutral lifecycle codes), resolve
-- the neutral handle, then call the frontend adapter. Never raises.
function M.declaration_semantics(disc, handle)
    local ok, err = lease_check(disc)
    if not ok then return nil, err end
    local resolved = M.resolve_handle(disc, handle)
    if type(resolved) ~= "table" or type(resolved.frontend) ~= "table" or resolved.declaration == nil then
        return nil, diag("project.handle_invalid", "declaration handle does not resolve in this discovery")
    end
    local ok2, rec, ferr = pcall(resolved.frontend.declaration_semantics, resolved.declaration)
    if not ok2 then return nil, diag("project.internal", "frontend declaration_semantics raised: " .. tostring(rec)) end
    return rec, ferr
end

-- analyze.scope(disc, handle) -> (model, nil) | (nil, Diagnostic). Same lifecycle gate + boundary.
function M.scope(disc, handle)
    local ok, err = lease_check(disc)
    if not ok then return nil, err end
    local resolved = M.resolve_handle(disc, handle)
    if type(resolved) ~= "table" or type(resolved.frontend) ~= "table" or resolved.unit == nil then
        return nil, diag("project.handle_invalid", "handle does not resolve to a unit in this discovery")
    end
    local ok2, model_or_nil, ferr = pcall(resolved.frontend.scope, resolved.unit)
    if not ok2 then return nil, diag("project.internal", "frontend scope raised: " .. tostring(model_or_nil)) end
    return model_or_nil, ferr
end

-- analyze.close(disc): frontend-neutral, idempotent generation teardown. Closes any live
-- per-language runtime lease, marks the lease closed, and logically invalidates BOTH Lua and
-- JavaScript semantic access (via open=false). Safe on a default/non-retained discovery and safe
-- to call twice.
function M.close(disc)
    local lease = type(disc) == "table" and disc._frontend_lease or nil
    if type(lease) ~= "table" then return end
    if lease.open == true and type(lease.sessions) == "table" then
        for langname, token in pairs(lease.sessions) do
            pcall(function() tool.frontend_close(langname, token) end)
        end
    end
    lease.open = false
    lease.sessions = {}
end

return M
