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
-- analyzed source file of its language; an application source plugin runs
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

-- opts: analyze(root, opts) -> discovery, store, exe, sha256_hex, no_cache,
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

    local analyze = opts.analyze or require("hull.project.analyze").analyze
    local disc = analyze(root, { source_hash = sha256_hex })
    if not disc or not disc.valid then
        local d = disc and disc.diagnostics and disc.diagnostics[1]
        return nil, "project discovery failed" .. (d and (": " .. tostring(d.message)) or "")
    end
    local stages, perr = plan.build(reg, disc, { sha256_hex = sha256_hex, source_hash = opts.source_hash })
    if not stages then return nil, perr end
    local store, own_store = opts.store, false
    if not store then
        store, err = default_store()
        if not store then return nil, err end
        own_store = true
    end
    local no_cache = opts.no_cache
    if no_cache == nil and tool then no_cache = tool.hull_cache_disabled(M.CACHE_ENV) end
    local exe = opts.exe or default_exe()

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
                    local a, aerr = action.new({ root = root, grants = st.grants, key = key, store = store,
                                                 limits = opts.limits })
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
                            source = { units = units },
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
                        r.status, r.error, r.detail = "failed", tok, detail
                        if a then r.diagnostics = a:get_diagnostics() end
                    end
                end
            end
        end
    end
    if own_store then store:close() end
    return results, nil, reg
end

return M
