--
-- hull.project.inspect — `hull agent inspect [app_dir]`: standalone project discovery.
--
-- Slice 2 (design: docs/project_discovery_design.md D8/D9/D11). Invokes the ONE canonical
-- analyzer (hull.project.analyze) on the app tree and emits an EXPLICIT, versioned JSON
-- projection to stdout. It does NOT re-scan or parse source itself. This is the
-- standalone path; the dev-running path (read a published generation) lands in Slice 3.
--
-- The projection is EXPLICIT: it copies only the public fields and DROPS every
-- generation-internal value -- the opaque `handle` on each decl, the internal `_by_source`
-- and `_handles` tables, and the by_id decl map (its ids are already in `annotated` +
-- each decl's `id`). Handles/state never reach the wire (D6).
--
-- Output: pure JSON on stdout (Hull routes `print` to stderr, so JSON goes via
-- tool.stdout). Exit 0 when a discovery was produced (validity is DATA in the JSON:
-- `valid` / `complete`); exit 2 only on a usage error.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local analyze = require("hull.project.analyze")
local json    = require("hull.json")

local function usage_error(msg)
    tool.stderr("hull agent inspect: " .. msg .. "\n")
    tool.stderr("usage: hull agent inspect [app_dir]\n")
    tool.exit(2)
end

-- Public projection of one normalized annotation (keeps provenance; `raw` retained -- it
-- is the exact source text, useful to a consumer, and carries no internal state).
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

-- Explicit public projection of the whole ProjectDiscovery. Only named public fields are
-- copied; _by_source / _handles / per-decl handle / the by_id decl map are omitted.
local function project(disc)
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

local function main()
    local app_dir = "."
    for i = 1, #arg do
        local a = arg[i]
        if a == "-h" or a == "--help" then
            tool.stdout("usage: hull agent inspect [app_dir]\n"); tool.exit(0)
        elseif a:sub(1, 1) == "-" and a ~= "-" then
            -- `--json` is the default + only format, accepted for symmetry; any other flag errors.
            if a ~= "--json" then usage_error("unknown flag: " .. a) end
        else
            app_dir = a
        end
    end

    -- The canonical analyzer never raises: it always returns a discovery (an invalid one
    -- on a bad root / internal defect). The command therefore succeeds (exit 0); the
    -- consumer reads `valid` / `complete` from the JSON.
    local disc = analyze.analyze(app_dir, { source_kind = "standalone" })
    tool.stdout(json.encode(project(disc)) .. "\n")
    tool.exit(0)
end

main()
