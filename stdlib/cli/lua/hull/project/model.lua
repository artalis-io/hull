--
-- hull.project.model — the frontend-neutral ProjectDiscovery model + indexes + IDs.
--
-- Design: docs/project_discovery_design.md D4/D5/D6. Assembles per-source frontend facts
-- into a normalized ProjectDiscovery: deterministic textual IDs, per-name declaration
-- facts sharing a group_id (annotations carry target_group_id), annotated-only PUBLIC
-- declarations[] with full per-source data retained internally (_by_source), the
-- valid/complete axes, indexes, and a summary. No Lua AST knowledge (it consumes the
-- frontend adapter's already-normalized facts).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local registry = require("hull.project.registry")

local M = { SCHEMA_VERSION = 1 }

local function range_tok(r) return r and (tostring(r.start) .. "-" .. tostring(r.stop)) or "0-0" end

-- Per-NAME id (D5): unique per declaration fact. group_id (D4): the declaration NODE's
-- identity, shared by every name of a multi-name declaration. Deterministic, textual,
-- no pointers; a within-generation key (not stable across arbitrary edits).
local function decl_id(language, path, kind, name, range)
    return table.concat({ language, path, kind, name or "?", range_tok(range) }, ":")
end
local function group_id(language, path, kind, group)
    return table.concat({ language, path, kind, "g" .. range_tok(group) }, ":")
end

local function count_status(sources, status)
    local n = 0
    for _, s in ipairs(sources) do if s.status == status then n = n + 1 end end
    return n
end

local function lang_counts(sources)
    local by = {}
    for _, s in ipairs(sources) do by[s.language] = (by[s.language] or 0) + 1 end
    return by
end

-- build(root, generation, per_source, op_diags, source_kind) -> ProjectDiscovery.
--   per_source[i] = { path, language, role, status, declarations[]?, diagnostics[]? }
--     declaration fact = { language, path, kind, name, range, group_range, is_method?,
--                          handle?, annotations[] = { name, args?, value?, raw, range } }
--   op_diags: discovery-level (operational) diagnostics.
--   source_kind: "standalone" | "dev" provenance marker.
function M.build(root, generation, per_source, op_diags, source_kind)
    local disc = {
        schema_version = M.SCHEMA_VERSION,
        generation     = generation or 0,
        source         = source_kind or "standalone",
        project_root   = root,
        valid          = true,
        complete       = true,
        sources        = {},
        declarations   = {},          -- PUBLIC: annotated facts only (D4)
        diagnostics    = {},
        frontends      = registry.frontends(),
        _by_source     = {},          -- INTERNAL: ALL decls (annotated + not), per source
        indexes        = {},
        summary        = {},
    }

    for _, d in ipairs(op_diags or {}) do
        disc.diagnostics[#disc.diagnostics + 1] = d
        if d.severity == "error" then disc.valid = false end
    end

    local total, annotated = 0, 0
    local by_annotation, by_source, by_language, by_id, annotated_ids = {}, {}, {}, {}, {}

    for _, s in ipairs(per_source) do
        disc.sources[#disc.sources + 1] =
            { path = s.path, language = s.language, role = s.role or "app", status = s.status }
        if s.status == "unsupported" then disc.complete = false end   -- unsupported app source (D6/D11)
        if s.status == "error" then disc.valid = false end            -- unreadable / parse-failed source
        for _, d in ipairs(s.diagnostics or {}) do
            disc.diagnostics[#disc.diagnostics + 1] = d
            if d.severity == "error" then disc.valid = false end       -- malformed source (D7)
        end

        disc._by_source[s.path] = disc._by_source[s.path] or {}
        for _, f in ipairs(s.declarations or {}) do
            total = total + 1
            local gid = group_id(f.language, f.path, f.kind, f.group_range)
            local id  = decl_id(f.language, f.path, f.kind, f.name, f.range)
            local anns = {}
            for _, a in ipairs(f.annotations or {}) do
                anns[#anns + 1] = { name = a.name, args = a.args, value = a.value, raw = a.raw,
                                    range = a.range, target_group_id = gid, frontend = f.language }
            end
            local decl = { id = id, group_id = gid, language = f.language, path = f.path,
                           kind = f.kind, name = f.name, range = f.range,
                           is_method = f.is_method or nil, annotations = anns,
                           status = "declared", handle = f.handle }
            disc._by_source[s.path][#disc._by_source[s.path] + 1] = decl

            if #anns > 0 then
                annotated = annotated + 1
                disc.declarations[#disc.declarations + 1] = decl
                by_id[id] = decl
                annotated_ids[#annotated_ids + 1] = id
                by_source[f.path] = by_source[f.path] or {}
                by_source[f.path][#by_source[f.path] + 1] = id
                by_language[f.language] = by_language[f.language] or {}
                by_language[f.language][#by_language[f.language] + 1] = id
                local seen_names = {}
                for _, a in ipairs(anns) do
                    if not seen_names[a.name] then     -- one id per annotation NAME per decl
                        seen_names[a.name] = true
                        by_annotation[a.name] = by_annotation[a.name] or {}
                        by_annotation[a.name][#by_annotation[a.name] + 1] = id
                    end
                end
            end
        end
    end

    disc.indexes = { by_annotation = by_annotation, by_source = by_source,
                     by_language = by_language, by_id = by_id, annotated = annotated_ids }
    disc.summary = {
        sources_total       = #disc.sources,
        sources_analyzed    = count_status(disc.sources, "analyzed"),
        sources_unsupported = count_status(disc.sources, "unsupported"),
        declarations_total     = total,
        declarations_annotated = annotated,
        by_language = lang_counts(disc.sources),
    }
    return disc
end

return M
