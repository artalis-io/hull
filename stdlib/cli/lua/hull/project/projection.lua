--
-- hull.project.projection — the ONE public wire-schema projection of a ProjectDiscovery.
--
-- Side-effect-free (requiring this module runs NO CLI): both `hull agent inspect`
-- (standalone) and `hull dev`'s discovery.json publication call M.project so
-- there is a SINGLE definition of the serialized shape. It copies only named public
-- fields and DROPS every generation-internal value -- the per-declaration opaque `handle`,
-- the internal `_by_source` and `_handles` tables, and the `by_id` decl map (its ids are
-- already in `annotated` + each decl's `id`). Design: docs/project_discovery_design.md D6.
--
-- Returns a plain serializable Lua table (the caller json.encodes it). Never raises.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- Public projection of one normalized annotation (keeps provenance; `raw` retained -- it
-- is exact source text and carries no internal state).
local function project_annotation(a)
    return { name = a.name, args = a.args, value = a.value, raw = a.raw, range = a.range,
             target_group_id = a.target_group_id, frontend = a.frontend }
end

-- Public projection of one declaration fact -- explicitly WITHOUT `handle` (generation
-- -internal; resolvable only in-process via analyze.resolve_handle, never serialized).
local function project_decl(d)
    local anns = {}
    for _, a in ipairs(d.annotations or {}) do anns[#anns + 1] = project_annotation(a) end
    return { id = d.id, group_id = d.group_id, language = d.language, path = d.path,
             kind = d.kind, name = d.name, range = d.range, is_method = d.is_method,
             status = d.status, annotations = anns }
end

-- project(disc) -> plain serializable table. Only named public fields; _by_source /
-- _handles / per-decl handle / the by_id decl map are omitted.
function M.project(disc)
    local decls = {}
    for _, d in ipairs(disc.declarations or {}) do decls[#decls + 1] = project_decl(d) end
    local idx = disc.indexes or {}
    return {
        schema_version = disc.schema_version,
        generation     = disc.generation,
        source         = disc.source,
        project_root   = disc.project_root,
        valid          = disc.valid,
        complete       = disc.complete,
        sources        = disc.sources,
        frontends      = disc.frontends,
        declarations   = decls,
        diagnostics    = disc.diagnostics,
        summary        = disc.summary,
        indexes        = {
            by_annotation = idx.by_annotation,
            by_source     = idx.by_source,
            by_language   = idx.by_language,
            annotated     = idx.annotated,
        },
    }
end

return M
