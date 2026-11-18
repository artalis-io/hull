-- luacheck: globals T
-- The pipeline plan (hull.buildctx.plan, docs/buildcontext_design.md §13.7):
-- a Hull route over whole source files of its language, keyed by the bytes
-- discovery analyzed, before the application plugins; then a real route run
-- through hull.buildctx.pipeline - a bundled plugin on a test route, the
-- hull binary T.hull running tests/fixtures/build_route_js - and the
-- generation its output becomes. Run by tests/hull/buildctx/test_buildctx.c,
-- which provides T and a temporary HOME.

local plan = require("hull.buildctx.plan")
local registry = require("hull.buildctx.registry")

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1 else fail = fail + 1; io.stderr:write("  FAIL: " .. name .. "\n") end
end
local function eq(a, b, name)
    ok(a == b, name .. " (got " .. tostring(a) .. ", want " .. tostring(b) .. ")")
end
local function has(s, needle, name)
    ok(type(s) == "string" and s:find(needle, 1, true) ~= nil,
       name .. " (got " .. tostring(s) .. ", want ..." .. needle .. "...)")
end

local roots = {}
local function tmp(name) local d = T.tmpdir(name); roots[#roots + 1] = d; return d end

local ROUTE_ENTRY = T.cwd .. "/tests/fixtures/build_route_js/app.js"
local BUNDLED = { id = "route-demo", major = 1, entry = ROUTE_ENTRY, language = "javascript",
                  descriptor = { id = "route-demo", version = 1, input = "source",
                                 languages = { "typescript" }, output = "generated" } }
local ROUTE = { id = "typescript-demo", unit = "module", language = "typescript", plugin = "route-demo" }

-- A discovery with the given sources ({ path, language, status }) and
-- declarations, as hull.project.analyze makes it; hashes = path -> hash.
local function disc_of(sources, decls, hashes)
    return { valid = true, sources = sources, declarations = decls or {}, _source_hashes = hashes or {} }
end

-- ── the plan, over a registry with a route and application plugins ───
local root = tmp("plan")
local app_src = { identity = "app:plugins/a#x", path = "plugins/a", entry = root .. "/plugins/a/app.lua",
                  config = {}, descriptor = { id = "a", version = 1, input = "source",
                                              languages = { "typescript", "lua" }, output = "g" } }
local app_art = { identity = "app:plugins/b#y", path = "plugins/b", entry = root .. "/plugins/b/app.lua",
                  config = {}, descriptor = { id = "b", version = 1, input = "artifact", kinds = { "k" },
                                              output = "g" } }
local function reg_of(plugins)
    local reg = assert(registry.load(root, { bundled = { BUNDLED }, routes = { ROUTE }, sha256_hex = T.sha256 }))
    for _, p in ipairs(plugins or {}) do reg.plugins[#reg.plugins + 1] = p end
    return reg
end

local SOURCES = {
    { path = "lib/b.ts", language = "typescript", status = "analyzed" },
    { path = "app.ts", language = "typescript", status = "analyzed" },
    { path = "util.lua", language = "lua", status = "analyzed" },
    { path = "x.tsx", language = "typescript", status = "unsupported" },
    { path = "plugins/a/helper.ts", language = "typescript", status = "analyzed" },
}
local HASHES = { ["lib/b.ts"] = T.sha256("b"), ["app.ts"] = T.sha256("app"), ["util.lua"] = T.sha256("u"),
                 ["plugins/a/helper.ts"] = T.sha256("h") }
local DECLS = {
    { id = "d1", language = "typescript", path = "app.ts", kind = "function", name = "f",
      annotations = { { name = "query", value = "q" } }, range = { start = 0, stop = 3, line = 1, col = 1 } },
}

do
    local reg = reg_of({ app_src, app_art })
    local stages, err = plan.build(reg, disc_of(SOURCES, DECLS, HASHES), { sha256_hex = T.sha256 })
    ok(stages, "a plan (" .. tostring(err) .. ")")
    eq(#stages, 3, "a route stage, then the application plugins")
    local r, a, b = stages[1], stages[2], stages[3]
    eq(r.identity, "stdlib:route-demo@1", "the route runs its bundled plugin first")
    eq(r.route.id, "typescript-demo", "the stage names its route")
    eq(r.input, "module", "a route stage's input")
    eq(#r.units, 2, "every analyzed source of the route's language, outside plugin directories")
    eq(r.units[1].id, "module:typescript:app.ts", "units are sorted by id")
    eq(r.units[1].name, "./app.ts", "a module unit is named for its file")
    eq(r.units[1].kind, "module", "its kind")
    eq(r.units[1].sha256, HASHES["app.ts"], "it carries the analyzed bytes' hash")
    eq(r.units[2].path, "lib/b.ts", "the second file")
    eq(#r.grants, 0, "a route stage has no workspace grants")
    eq(#r.key, 64, "and an action key")
    eq(a.identity, "app:plugins/a#x", "the application source plugin after the routes")
    eq(#a.units, 1, "its annotated declarations")
    eq(a.grants[1], "app.ts", "its grants are its units' files")
    eq(b.skip ~= nil, true, "an artifact stage is skipped for now")

    -- The key follows the analyzed bytes, and only the route's.
    local h2 = {}
    for k, v in pairs(HASHES) do h2[k] = v end
    h2["app.ts"] = T.sha256("app, edited")
    local s2 = assert(plan.build(reg, disc_of(SOURCES, DECLS, h2), { sha256_hex = T.sha256 }))
    ok(s2[1].key ~= r.key, "an edited file changes the route's key")
    eq(s2[2].key, a.key, "the application plugin's key is its selection's")
    h2 = {}
    for k, v in pairs(HASHES) do h2[k] = v end
    h2["util.lua"] = T.sha256("lua, edited")
    s2 = assert(plan.build(reg, disc_of(SOURCES, DECLS, h2), { sha256_hex = T.sha256 }))
    eq(s2[1].key, r.key, "a file of another language leaves the route's key")

    -- The route is part of the key: the same plugin and units on another route differ.
    local other = plan.action_key(r.plugin, r.units, reg.config_sha256, T.sha256,
                                  { id = "typescript-other", kind = "module:typescript" })
    ok(other ~= r.key, "the route id is in the key")

    -- A source analyzed without a hash cannot be keyed.
    local missing = {}
    for k, v in pairs(HASHES) do missing[k] = v end
    missing["lib/b.ts"] = nil
    local none, merr = plan.build(reg, disc_of(SOURCES, DECLS, missing), { sha256_hex = T.sha256 })
    ok(none == nil, "a source without a hash refuses the plan")
    has(merr, "lib/b.ts was analyzed without a hash", "it names the file")

    -- No source of the route's language: the stage has no units.
    local s3 = assert(plan.build(reg, disc_of({ SOURCES[3] }, {}, HASHES), { sha256_hex = T.sha256 }))
    eq(#s3[1].units, 0, "no TypeScript, no units (the pipeline reports no_input)")
end

-- ── a real route run, through the pipeline ───────────────────────────
local native = require("hull.buildctx._plugin")
local probe = native.spawn(T.hull, ROUTE_ENTRY, require("hull.buildctx.plugin").PLUGIN_ARGS)
if probe == nil then
    io.stderr:write("  (no plugin channel on this host: the real route run is skipped)\n")
else
    probe:kill()
    local pipeline = require("hull.buildctx.pipeline")
    local gen = require("hull.buildctx.generation")
    local ws = tmp("plan_ws")
    T.mkdir(ws .. "/lib")
    local function put(rel, s) local f = assert(io.open(ws .. "/" .. rel, "wb")); f:write(s); f:close() end
    put("app.ts", "const x: number = 1;\n")
    put("lib/b.ts", "export const y = 2;\n")
    local srcs = {
        { path = "app.ts", language = "typescript", status = "analyzed" },
        { path = "lib/b.ts", language = "typescript", status = "analyzed" },
    }
    local hashes = { ["app.ts"] = T.sha256("const x: number = 1;\n"), ["lib/b.ts"] = T.sha256("export const y = 2;\n") }
    local store = assert(require("hull.buildctx._store").open(tmp("plan_store")))
    local function run(h, no_cache)
        return pipeline.run(ws, {
            bundled = { BUNDLED }, routes = { ROUTE }, sha256_hex = T.sha256, store = store,
            exe = T.hull, no_cache = no_cache or false,
            analyze = function(_, opts)
                ok(opts and opts.source_hash == T.sha256, "the pipeline asks discovery for hashes")
                return disc_of(srcs, {}, h)
            end,
        })
    end

    local results, err = run(hashes)
    ok(results, "a pipeline run with a route (" .. tostring(err) .. ")")
    local r = results and results[1] or {}
    eq(#(results or {}), 1, "one stage")
    eq(r.status, "committed", "the route's plugin ran and committed (" .. tostring(r.error) .. " " ..
                              tostring(r.detail) .. ")")
    eq(r.route, "typescript-demo", "the result names its route")
    eq(r.units, 2, "over both files")
    eq(#(r.outputs or {}), 2, "one generated output per file")

    local g, gerr = gen.from_results(results or {}, T.sha256)
    ok(g, "a generation from the route's result (" .. tostring(gerr) .. ")")
    if g then
        eq(#g.entries, 2, "two entries")
        eq(g.entries[1].name, "app.ts", "named for the file it shadows")
        eq(g.entries[1].bytes, "// app.ts " .. hashes["app.ts"] .. "\n", "the plugin's bytes")
        eq(g.entries[1].position_map, "identity", "its position map")
        eq(g.entries[1].origin.plugin, "stdlib:route-demo@1", "its origin")
        eq(g.entries[2].name, "lib/b.ts", "the second file")
        eq(#g.inputs, 0, "a route stage observes no workspace path")
    end

    local again = assert(run(hashes))
    eq(again[1].status, "cached", "the same bytes reuse the committed result")
    eq(again[1].key, r.key, "under the same key")

    local edited = { ["app.ts"] = T.sha256("const x: number = 2;\n"), ["lib/b.ts"] = hashes["lib/b.ts"] }
    local third = assert(run(edited))
    eq(third[1].status, "committed", "an edited file runs the route again")
    ok(third[1].key ~= r.key, "under a new key")

    local none = assert(pipeline.run(ws, {
        bundled = { BUNDLED }, routes = { ROUTE }, sha256_hex = T.sha256, store = store, exe = T.hull,
        no_cache = true, analyze = function() return disc_of({}, {}, {}) end,
    }))
    eq(none[1].status, "no_input", "no TypeScript: the route has no input")
    store:close()
end

for _, d in ipairs(roots) do T.rmtree(d) end
return { pass = pass, fail = fail }
