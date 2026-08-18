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

-- Project-relative path: strip the root prefix so `path` + IDs are stable + portable.
local function rel_path(root, path)
    if root == "." or root == "" then return path end
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

-- Build per-source declaration facts via the frontend CONTRACT (never the AST).
local function collect_decls(fe, unit, language, rel)
    local out = {}
    for _, d in ipairs(fe.declarations(unit)) do
        out[#out + 1] = {
            language    = language,
            path        = rel,
            kind        = fe.decl_kind(d),
            name        = fe.decl_name(d),
            range       = fe.decl_range(d),
            group_range = fe.decl_group_range(d),
            is_method   = fe.decl_is_method(d),
            handle      = fe.decl_handle(d),
            annotations = fe.decl_annotations(d),
        }
    end
    return out
end

-- analyze(root, opts?) -> ProjectDiscovery.
--   opts.generation  : the dev generation counter (default 0 = standalone).
--   opts.source_kind : "standalone" (default) | "dev" provenance marker.
-- Discovers APPLICATION source (known-language extensions, static/ pruned so browser
-- assets never count -- D6), dispatches each to its frontend, and assembles the model.
function M.analyze(root, opts)
    opts = opts or {}
    root = discover.normalize(root or ".")

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
                            declarations = collect_decls(fe, unit, row.language, rel),
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

    return model.build(root, opts.generation or 0, per_source, op_diags, opts.source_kind or "standalone")
end

return M
