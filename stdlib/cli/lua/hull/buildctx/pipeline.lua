-- hull.buildctx.pipeline - one pipeline run over an app's build plugins
-- (docs/buildcontext_design.md §9.2, §10): the registry, each source plugin's
-- selection from project discovery, its action key, and a committed result -
-- found in the store, or made by running the plugin.
--
-- Host-owned tool code; `hull plugins run` and, later, the planner call it.
--
--   local pipeline = require("hull.buildctx.pipeline")
--   local results, err = pipeline.run("/abs/app")
--   -- results[i] = { identity, status, units, key, outputs, diagnostics,
--   --                error?, detail?, result? }
--   -- result: the committed result itself (action.lookup's: outputs with
--   --         their bytes, and the dependency manifest) when status is
--   --         "cached" or "committed"
--   -- status: "committed" (ran now), "cached" (a fresh committed result),
--   --         "failed" (error = the runner's token), "no_input" (nothing
--   --         selected), "skipped" (detail says why)
--
-- The stages, their selections and their keys are the plan's
-- (hull.buildctx.plan): a Hull route runs its bundled plugin over every
-- analyzed source file of its language (TypeScript erasure: §13.8), the
-- discovery's frontend sessions answering its erased_source; an application source plugin runs
-- over every annotated declaration project discovery finds in a language it
-- lists, carrying an annotation it lists (any annotation when it lists none),
-- outside every plugin directory. Such a unit is a plain value: { id,
-- language, path, kind, name, range, annotations = { { name, value? } } } -
-- the same shape for every language, so one plugin serves Lua and JavaScript
-- sources alike, and the action's grants are exactly the units' files.
--
-- The action key is the SHA-256 of a canonical envelope: the key format and
-- the channel protocol version, the plugin's identity and descriptor, its
-- configuration, the selection (a route's units carry their files' hashes),
-- the route, and hull.build.json's hash. A plugin's code, its configuration
-- and the source it is shown all change it; what it reads besides is covered
-- by the result's dependency manifest (§10).

local action = require("hull.buildctx.action")
local outputs = require("hull.buildctx.outputs")
local plugin = require("hull.buildctx.plugin")
local plan = require("hull.buildctx.plan")
local registry = require("hull.buildctx.registry")

local M = {}

M.STORE_KIND = "build-results"     -- its hull cache kind
M.CACHE_ENV = "build"              -- HULL_NO_BUILD_CACHE

local function default_sha256_hex(s)
    return require("hull.encoding").hex.encode(require("hull.crypto").sha256(s))
end

local function default_store()
    local dir = tool.hull_cache_dir(M.STORE_KIND)
    if not dir then return nil, "the build result store is unavailable (no home directory)" end
    return require("hull.buildctx._store").open((dir:gsub("/$", "")))
end

local function default_exe()
    return tool.self_path() or __hull_exe   -- luacheck: ignore 113
end

-- A committed result's outputs and diagnostics, for the caller.
local function describe(res, sha256_hex)
    local outs = {}
    for i, o in ipairs(res.outputs) do
        local e = { declared = o.declared, descriptor = o.descriptor }
        if o.bytes then
            e.sha256, e.size = sha256_hex(o.bytes), #o.bytes
        else
            e.entries = {}
            for j, ent in ipairs(o.entries) do
                e.entries[j] = { path = ent.path, mode = ent.mode, sha256 = sha256_hex(ent.bytes),
                                 size = #ent.bytes }
            end
        end
        outs[i] = e
    end
    return outs, res.diagnostics
end

-- ": <path>:<line>:<col>: <code>: <message>" for an invalid discovery's
-- first error (a refused TypeScript construct, a syntax error), and how many
-- more there are.
local function discovery_error(disc)
    local first, more = nil, 0
    for _, d in ipairs(disc and disc.diagnostics or {}) do
        if d.severity == "error" then
            if first then more = more + 1 else first = d end
        end
    end
    if not first then return "" end
    local r = type(first.range) == "table" and first.range or {}
    local where = first.path and (tostring(first.path) .. (r.line and (":" .. r.line .. ":" .. (r.col or 1)) or "") .. ": ") or ""
    return ": " .. where .. tostring(first.code) .. ": " .. tostring(first.message) ..
           (more > 0 and (" (and " .. more .. " more)") or "")
end

-- A route stage's erased_source(unit id): the frontend operation over the unit's
-- analyzed bytes, its refusals naming the unit's file. Only the stage's own
-- units resolve.
local function route_erased(st, disc, erased_source)
    local by_id = {}
    for _, u in ipairs(st.units) do by_id[u.id] = u end
    return function(id)
        local u = by_id[id]
        if not u then return nil, "unknown_unit" end
        local r, d = erased_source(disc, u.path)
        if not r then
            if d and d.code == "project.operation_unsupported" then return nil, "unsupported_op" end
            return nil, "frontend_failed"
        end
        local refusals = {}
        for i, f in ipairs(r.refusals or {}) do
            local rg = type(f.range) == "table" and f.range or nil
            refusals[i] = { severity = "error", code = tostring(f.code), message = tostring(f.message),
                            path = u.path,
                            range = rg and { start = rg.start, stop = rg.stop, line = rg.line, col = rg.col } or nil }
        end
        return { bytes = r.bytes, refusals = refusals }
    end
end

-- opts: analyze(root, opts) -> discovery, close(discovery), erased_source(disc,
-- path) (hull.project.analyze's by default), store, exe, sha256_hex, no_cache,
-- limits, source_hash (the plan's), and the registry's (extract, fs, native,
-- bundled, routes).
function M.run(root, opts)
    opts = opts or {}
    local sha256_hex = opts.sha256_hex or default_sha256_hex
    local reg, err = registry.load(root, {
        extract = opts.extract, fs = opts.fs, native = opts.native,
        sha256_hex = sha256_hex, bundled = opts.bundled, routes = opts.routes,
    })
    if not reg then return nil, err end
    local results = {}
    if #reg.plugins == 0 then return results, nil, reg end

    local A = require("hull.project.analyze")
    local analyze = opts.analyze or A.analyze
    -- A route's plugin asks for frontend operations over the analyzed bytes,
    -- so the discovery keeps its frontend sessions while the run lasts.
    local retain = #(reg.routes or {}) > 0
    local disc = analyze(root, { source_hash = sha256_hex, retain_frontend = retain or nil })
    local function close() if retain and disc then pcall(opts.close or A.close, disc) end end
    if not disc or not disc.valid then
        close()
        return nil, "project discovery failed" .. discovery_error(disc)
    end
    local stages, perr = plan.build(reg, disc, { sha256_hex = sha256_hex, source_hash = opts.source_hash })
    if not stages then close(); return nil, perr end
    local store, own_store = opts.store, false
    if not store then
        store, err = default_store()
        if not store then close(); return nil, err end
        own_store = true
    end
    local erased_source = opts.erased_source or A.erased_source
    local no_cache = opts.no_cache
    if no_cache == nil and tool then no_cache = tool.hull_cache_disabled(M.CACHE_ENV) end
    local exe = opts.exe or default_exe()
    -- A bundled plugin's files go into a private directory for this run.
    local dirs = {}
    local function cleanup()
        for _, d in ipairs(dirs) do tool.rmdir(d) end
        close()
    end

    for n, st in ipairs(stages) do
        local p = st.plugin
        local r = { identity = st.identity, route = st.route and st.route.id or nil }
        results[#results + 1] = r
        if st.skip then
            r.status, r.detail = "skipped", st.skip
        else
            local units, key = st.units, st.key
            r.units, r.key = #units, key
            if not key then
                r.status, r.error = "failed", "unencodable_selection"
            elseif #units == 0 then
                r.status = "no_input"
            else
                local hit = not no_cache and action.lookup(store, key, root)
                if hit then
                    r.status, r.result = "cached", hit
                    r.outputs, r.diagnostics = describe(hit, sha256_hex)
                else
                    local a, aerr
                    local dir, merr = registry.materialize(p)
                    if dir then dirs[#dirs + 1] = dir end
                    if merr then
                        aerr = "spawn_failed"
                        r.detail = merr
                    else
                        a, aerr = action.new({ root = root, grants = st.grants, key = key, store = store,
                                               limits = opts.limits })
                    end
                    local runner = a and plugin.new({ exe = exe, entry = p.entry, limits = opts.limits },
                                                    { native = opts.native })
                    local ok, tok, detail
                    if not a then
                        tok = aerr
                    elseif not runner then
                        tok = "invalid_args"
                    else
                        ok, tok, detail = runner:run(a, {
                            id = n, config = p.config, meta = { input = st.input },
                            source = { units = units,
                                       erased_source = st.route and route_erased(st, disc, erased_source) },
                        })
                        runner:close()
                    end
                    if ok then
                        local res = action.lookup(store, key, root)
                        if res then
                            r.status, r.result = "committed", res
                            r.outputs, r.diagnostics = describe(res, sha256_hex)
                        else
                            r.status, r.error = "failed", "store_failed"
                        end
                    else
                        r.status, r.error, r.detail = "failed", tok, detail or r.detail
                        if a then r.diagnostics = a:get_diagnostics() end
                    end
                end
            end
        end
    end
    if own_store then store:close() end
    cleanup()
    return results, nil, reg
end

return M
