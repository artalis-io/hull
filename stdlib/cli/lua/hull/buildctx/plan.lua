-- hull.buildctx.plan - the pipeline plan (docs/buildcontext_design.md §13.7):
-- which stages a pipeline run has, in which order, each with its selection and
-- its action key. A Hull-defined route runs a bundled plugin over every
-- analyzed source file of its language; an application plugin runs over the
-- annotated declarations it lists.
--
-- Host-owned tool code; hull.buildctx.pipeline runs the plan.
--
--   local plan = require("hull.buildctx.plan")
--   local stages, err = plan.build(reg, disc, { sha256_hex = fn })
--   -- stages[i] = { identity, plugin, route?, input, units, grants, key?,
--   --               skip? }
--   -- input: "module" (a route's), "source" or "artifact"
--   -- skip:  why the stage does not run (artifact stages, for now)
--
-- Order: the routes first, by route id, then the application plugins in
-- hull.build.json's order. Every stage is a source stage reading only the
-- workspace, so none depends on another and the order is fixed, not
-- scheduled; artifact routes and their DAG (ordering by artifact kind, cycle
-- rejection) are designed, not built.
--
-- A route stage's unit is a whole file: { id = "module:<language>:<path>",
-- language, path, kind = "module", name = "./<path>", sha256 } - sha256 being
-- the hash of the bytes discovery analyzed (analyze.source_hash), so the key
-- changes with the file. It has no grants: what the plugin is shown comes
-- from the host (the unit, and frontend operations over the analyzed bytes),
-- never from a read of the workspace. A route over a language with no
-- analyzed source has no units, and the stage reports no_input.

local outputs = require("hull.buildctx.outputs")

local M = {}

M.KEY_FORMAT = "hull-build-action-key"
M.KEY_VERSION = 1
M.PROTOCOL = 1

local function under(path, dirs)
    for _, d in ipairs(dirs) do
        if path:sub(1, #d + 1) == d .. "/" then return true end
    end
    return false
end

local function set_of(list)
    if not list or #list == 0 then return nil end
    local s = {}
    for _, v in ipairs(list) do s[v] = true end
    return s
end

-- An application source plugin's selection: its units and grants.
function M.select(discovery, desc, plugin_dirs)
    local langs, anns = set_of(desc.languages), set_of(desc.annotations)
    local units, grants, seen = {}, {}, {}
    for _, d in ipairs(discovery.declarations or {}) do
        if langs[d.language] and not under(d.path, plugin_dirs) then
            local kept = {}
            for _, a in ipairs(d.annotations or {}) do
                if not anns or anns[a.name] then
                    kept[#kept + 1] = { name = a.name, value = a.value }
                end
            end
            if #kept > 0 then
                local r = d.range or {}
                units[#units + 1] = {
                    id = d.id, language = d.language, path = d.path, kind = d.kind,
                    name = d.name, annotations = kept,
                    range = { start = r.start, stop = r.stop, line = r.line, col = r.col },
                }
                if not seen[d.path] then
                    seen[d.path] = true
                    grants[#grants + 1] = d.path
                end
            end
        end
    end
    table.sort(units, function(a, b) return a.id < b.id end)
    table.sort(grants)
    return units, grants
end

-- A route's selection: every analyzed source file of its language, whole.
-- source_hash(path) -> the analyzed bytes' hash. nil, path when one has none.
function M.select_route(discovery, route, plugin_dirs, source_hash)
    local units = {}
    for _, s in ipairs(discovery.sources or {}) do
        if s.language == route.language and s.status == "analyzed" and not under(s.path, plugin_dirs) then
            local h = source_hash(s.path)
            if not h then return nil, s.path end
            units[#units + 1] = { id = "module:" .. s.language .. ":" .. s.path, language = s.language,
                                  path = s.path, kind = "module", name = "./" .. s.path, sha256 = h }
        end
    end
    table.sort(units, function(a, b) return a.id < b.id end)
    return units
end

function M.action_key(p, units, config_sha256, sha256_hex, route)
    local env = outputs.encode({
        format = M.KEY_FORMAT, version = M.KEY_VERSION, protocol = M.PROTOCOL,
        plugin = p.identity, descriptor = p.descriptor, config = p.config,
        units = units, build_config = config_sha256 or "",
        route = route and { id = route.id, kind = route.kind } or nil,
    })
    if not env then return nil end
    return sha256_hex(env)
end

-- opts: sha256_hex, source_hash(path) (default: analyze.source_hash over disc).
function M.build(reg, disc, opts)
    opts = opts or {}
    local sha256_hex = opts.sha256_hex
    local source_hash = opts.source_hash or function(path)
        return require("hull.project.analyze").source_hash(disc, path)
    end
    local plugin_dirs = {}
    for _, p in ipairs(reg.plugins) do
        if p.path then plugin_dirs[#plugin_dirs + 1] = p.path end
    end
    local stages = {}
    for _, r in ipairs(reg.routes or {}) do
        local units, missing = M.select_route(disc, r, plugin_dirs, source_hash)
        if not units then return nil, "route " .. r.id .. ": " .. missing .. " was analyzed without a hash" end
        stages[#stages + 1] = { identity = r.plugin.identity, plugin = r.plugin, route = r, input = "module",
                                units = units, grants = {},
                                key = M.action_key(r.plugin, units, reg.config_sha256, sha256_hex, r) }
    end
    for _, p in ipairs(reg.plugins) do
        if not p.bundled then
            local st = { identity = p.identity, plugin = p, input = p.descriptor.input }
            if p.descriptor.input ~= "source" then
                st.skip = "artifact stages do not run yet"
            else
                st.units, st.grants = M.select(disc, p.descriptor, plugin_dirs)
                st.key = M.action_key(p, st.units, reg.config_sha256, sha256_hex)
            end
            stages[#stages + 1] = st
        end
    end
    return stages
end

return M
