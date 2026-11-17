--
-- test_project.lua - tests for the project source-discovery layer.
--
-- Covers the Lua frontend adapter (declarations/annotations/ranges without executing
-- app source), the frontend-neutral model (IDs, indexes, valid/complete, annotated-only
-- public retention), the registry (honest capabilities), and the analyze orchestrator
-- (discovery dispatch, static/ pruning, unsupported JS -> complete=false, malformed ->
-- valid=false, determinism). Pure Lua; run by tests/hull/source/test_lua_source.c. The
-- orchestrator's tool.find_files/read_file are stubbed over an in-memory fixture tree.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local frontend = require("hull.project.frontend_lua")
local registry = require("hull.project.registry")
local model    = require("hull.project.model")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

-- Find the first frontend decl for a given name (adapter-level).
local function decl_named(fe, unit, name)
    for _, d in ipairs(fe.declarations(unit)) do
        if fe.decl_name(d) == name then return d end
    end
    return nil
end

-- ── frontend adapter: declarations without executing app source ──────
do
    local unit = frontend.parse("local x = 1\nreturn x", "t.lua")
    ok(unit ~= nil, "adapter parses clean Lua")
    local dx = decl_named(frontend, unit, "x")
    ok(dx ~= nil, "local decl 'x' discovered")
    eq(frontend.decl_kind(dx), "local", "'x' kind is local")
    ok(frontend.decl_range(dx) and frontend.decl_range(dx).start ~= nil, "decl carries a range")
    ok(frontend.decl_range(dx).line ~= nil, "decl range carries line/col")
end

-- ── dotted / method / local-function normalization ──────────────────
do
    local unit = frontend.parse(
        "local function f() end\nfunction a.b.c() end\nfunction a:m() end\n", "t.lua")
    local f = decl_named(frontend, unit, "f")
    eq(frontend.decl_kind(f), "local_function", "local function kind")
    local abc = decl_named(frontend, unit, "a.b.c")
    ok(abc ~= nil, "dotted function name normalized to 'a.b.c'")
    eq(frontend.decl_kind(abc), "function", "dotted function kind is function")
    ok(not frontend.decl_is_method(abc), "dotted function is not a method")
    local am = decl_named(frontend, unit, "a:m")
    ok(am ~= nil, "method function name normalized to 'a:m'")
    ok(frontend.decl_is_method(am), "method function flagged is_method")
end

-- ── multi-name local: two facts, one shared group range ─────────────
do
    local unit = frontend.parse("local a, b = 1, 2\nreturn a + b", "t.lua")
    local da, db = decl_named(frontend, unit, "a"), decl_named(frontend, unit, "b")
    ok(da ~= nil and db ~= nil, "both names of 'local a, b' discovered as facts")
    ok(frontend.decl_range(da).start ~= frontend.decl_range(db).start, "each name has its OWN range")
    local ga, gb = frontend.decl_group_range(da), frontend.decl_group_range(db)
    ok(ga.start == gb.start and ga.stop == gb.stop, "both names share ONE declaration group range")
end

-- ── annotations attached (known + unknown survive) ──────────────────
do
    local unit = frontend.parse(
        "---@type Widget\nlocal w = {}\n---@wild(1) freeform text\nlocal y = 2\nreturn w, y", "t.lua")
    local dw = decl_named(frontend, unit, "w")
    local aw = frontend.decl_annotations(dw)
    ok(#aw == 1 and aw[1].name == "type", "known @type annotation attached to 'w'")
    eq(aw[1].value, "Widget", "annotation value captured")
    local dy = decl_named(frontend, unit, "y")
    local ay = frontend.decl_annotations(dy)
    ok(#ay == 1 and ay[1].name == "wild", "UNKNOWN @wild annotation survives (not an error)")
    eq(ay[1].args, "1", "unknown annotation args captured (inner content)")
end

-- ── malformed Lua: parse still returns, diagnostics are errors ──────
do
    local unit, diags = frontend.parse("local = = )", "bad.lua")
    -- Either a returned unit with error diagnostics, or (nil, {internal}); both are errors.
    local has_error = false
    for _, d in ipairs(diags or {}) do if d.severity == "error" then has_error = true end end
    ok(has_error, "malformed Lua yields error diagnostics (never a clean parse)")
    if unit ~= nil then
        ok(#diags > 0, "malformed unit carries diagnostics")
    end
end

-- ── model: IDs, group sharing, annotated-only public, valid/complete ─
do
    -- Two facts of one multi-name group: one annotated, one not.
    local grp = { start = 1, stop = 12 }
    local per_source = { {
        path = "app.lua", language = "lua", role = "app", status = "analyzed",
        declarations = {
            { language = "lua", path = "app.lua", kind = "local", name = "a",
              range = { start = 7, stop = 8 }, group_range = grp, annotations = {
                { name = "type", value = "Foo" } } },
            { language = "lua", path = "app.lua", kind = "local", name = "b",
              range = { start = 10, stop = 11 }, group_range = grp, annotations = {} },
        },
        diagnostics = {},
    } }
    local disc = model.build("app", 0, per_source, {}, "standalone")
    eq(disc.schema_version, 1, "model schema_version is 1")
    eq(disc.summary.declarations_total, 2, "totals retain ALL declarations")
    eq(disc.summary.declarations_annotated, 1, "annotated count is 1")
    eq(#disc.declarations, 1, "PUBLIC declarations[] is annotated-only")
    eq(disc.declarations[1].name, "a", "the annotated fact 'a' is public")
    ok(disc._by_source["app.lua"] and #disc._by_source["app.lua"] == 2,
        "internal _by_source retains BOTH facts (annotated + not)")
    local da = disc.declarations[1]
    ok(da.id:find("app.lua") and da.id:find(":a:"), "id is deterministic + textual")
    -- group_id shared by both names (same group_range) even though only one is public
    local ga = da.group_id
    local gb = disc._by_source["app.lua"][2].group_id
    eq(ga, gb, "both names share one group_id")
    eq(da.annotations[1].target_group_id, ga, "annotation carries target_group_id = group_id")
    eq(da.annotations[1].frontend, "lua", "annotation carries frontend provenance")
    ok(disc.indexes.by_annotation["type"] and disc.indexes.by_annotation["type"][1] == da.id,
        "by_annotation index points at the annotated decl")
    eq(disc.indexes.by_id[da.id].name, "a", "by_id index resolves the decl")
    ok(disc.valid and disc.complete, "clean facts -> valid + complete")
end

-- ── model: unsupported source -> complete=false; error diag -> valid=false
do
    local d1 = model.build("app", 0, { { path = "x.js", language = "javascript",
        role = "app", status = "unsupported", diagnostics = {} } }, {}, "standalone")
    ok(not d1.complete, "an unsupported application source -> complete=false")
    ok(d1.valid, "unsupported does NOT by itself make it invalid")
    local d2 = model.build("app", 0, { { path = "bad.lua", language = "lua", role = "app",
        status = "error", diagnostics = { { severity = "error", code = "lua.syntax", message = "x" } } } },
        {}, "standalone")
    ok(not d2.valid, "an error diagnostic -> valid=false")
end

-- ── registry: honest capabilities + reserved JS ─────────────────────
do
    local exts = registry.known_exts()
    eq(table.concat(exts, ","), "cjs,cts,js,lua,mjs,mts,ts,tsx", "known_exts sorted + reserves JS / TS variants")
    ok(registry.for_ext("lua").analyzable, "lua is analyzable")
    ok(not registry.for_ext("js").analyzable, "js is known but NOT analyzable")
    ok(registry.for_ext("py") == nil, "unknown extension is not registered")
    local fr = registry.frontends()
    local lua_e, js_e
    for _, e in ipairs(fr) do
        if e.language == "lua" then lua_e = e elseif e.language == "javascript" then js_e = e end
    end
    ok(lua_e and lua_e.analyzable and #lua_e.capabilities == 5, "lua frontend reports its 5 shipped capabilities")
    local caps = {}; for _, c in ipairs(lua_e.capabilities) do caps[c] = true end
    ok(caps["semantics"], "lua frontend advertises the 'semantics' capability")
    ok(js_e and not js_e.analyzable and #js_e.capabilities == 0, "javascript reserved: analyzable=false, no capabilities")
    -- load() requires only the frontends FRONTENDS names: a row is caller data, and
    -- the require runs with the stdlib's identity (round-7 H3 reached hull._template
    -- and hull.db._internal_conn through it).
    ok(registry.load({ analyzable = true, frontend_module = "hull._template" }) == nil,
       "load() refuses a module name that is not a registered frontend")
    ok(registry.load({ analyzable = true, frontend_module = "hull.db._internal_conn" }) == nil,
       "load() refuses the internal-connection module")
    ok(registry.load(registry.for_ext("lua")) ~= nil, "load() still loads the lua frontend")
    -- TypeScript: .ts / .mts go to the TS frontend (engine-gated like JS); .tsx / .cts are known
    -- and never analyzable, even with the engine in, and are listed apart in the summary.
    eq(registry.for_ext("ts").language, "typescript", ".ts is the typescript language")
    eq(registry.for_ext("mts").frontend_module, "hull.project.frontend_typescript", ".mts -> the TS frontend")
    ok(not registry.for_ext("ts").analyzable, "without the JS engine .ts is not analyzable")
    local saved = _G.tool
    _G.tool = { frontend_available = function(lang) return lang == "javascript" end }
    ok(registry.for_ext("ts").analyzable and registry.for_ext("mts").analyzable, "with the engine .ts / .mts analyze")
    ok(not registry.for_ext("tsx").analyzable and not registry.for_ext("cts").analyzable,
       ".tsx / .cts stay unanalyzable with the engine in")
    ok(registry.load(registry.for_ext("tsx")) == nil, "no frontend loads for .tsx")
    local ts_e
    for _, e in ipairs(registry.frontends()) do if e.language == "typescript" then ts_e = e end end
    _G.tool = saved
    ok(ts_e and ts_e.analyzable, "typescript summary is analyzable (decided by .ts / .mts)")
    eq(ts_e and table.concat(ts_e.extensions, ","), "mts,ts", "typescript summary extensions")
    eq(ts_e and table.concat(ts_e.unsupported_extensions, ","), "cts,tsx", "typescript unsupported extensions")
    local tcaps = {}
    for _, c in ipairs(ts_e and ts_e.capabilities or {}) do tcaps[c] = true end
    ok(tcaps.type_erasure and tcaps.scope and tcaps.semantics, "typescript advertises type_erasure + the JS five")
end

-- ── orchestrator end-to-end (stubbed tool over an in-memory tree) ────
-- A minimal fake filesystem so analyze() runs its full discovery + dispatch path.
-- Fixture paths are treated as already-canonical (realpath is identity for anything that
-- "exists"; path_kind reports dir for a prefix-of-some-file, file for an exact file).
local function make_tool(fs)
    local function is_file(p) return fs[p] ~= nil end
    local function is_dir(p)
        if p == "." or p == "" then return true end
        for path in pairs(fs) do if path:sub(1, #p + 1) == p .. "/" then return true end end
        return false
    end
    return {
        find_files = function(root, pat, opts)
            local ext = pat:match("%*%.(.+)$")
            local excl = {}
            for _, e in ipairs((opts and opts.exclude_dirs) or {}) do excl[e] = true end
            local out = {}
            for path in pairs(fs) do
                local under = (root == "." or root == "") or (path:sub(1, #root + 1) == root .. "/")
                if under and path:sub(- (#ext + 1)) == "." .. ext then
                    local skip = false
                    for seg in path:gmatch("[^/]+") do if excl[seg] then skip = true; break end end
                    if not skip then out[#out + 1] = path end
                end
            end
            table.sort(out)
            return out
        end,
        read_file = function(path) return fs[path] end,
        realpath  = function(p)
            if is_file(p) or is_dir(p) then return p end
            return nil, "missing"
        end,
        path_kind = function(p)
            if is_file(p) then return "file" elseif is_dir(p) then return "dir" end
            return nil
        end,
    }
end

do
    -- require analyze AFTER stubbing is fine: it reads the `tool` global at call time.
    local analyze = require("hull.project.analyze")
    local FS = {
        ["app/app.lua"]            = "---@route\nlocal handler = function() end\nreturn handler\n",
        ["app/routes/users.lua"]   = "---@resource\nlocal function list() end\nreturn list\n",
        ["app/static/vendor.js"]   = "window.x = 1;",     -- browser asset: must be PRUNED
        ["app/client.js"]          = "export const q = 1;", -- app JS: unsupported
        ["app/broken.lua"]         = "local = = )",         -- malformed: valid=false
    }
    _G.tool = make_tool(FS)
    local disc = analyze.analyze("app")
    _G.tool = nil

    -- static/vendor.js must NOT appear as a source (pruned); the app JS must.
    local paths = {}
    for _, s in ipairs(disc.sources) do paths[s.path] = s.status end
    ok(paths["static/vendor.js"] == nil, "static/*.js browser asset is pruned (not application source)")
    eq(paths["client.js"], "unsupported", "application .js is honestly unsupported (not parsed as Lua)")
    eq(paths["app.lua"], "analyzed", "app.lua analyzed")
    eq(paths["routes/users.lua"], "analyzed", "nested routes/users.lua analyzed")
    eq(paths["broken.lua"], "error", "malformed broken.lua is an error source")

    ok(not disc.complete, "an application .js makes the generation complete=false")
    ok(not disc.valid, "a malformed Lua source makes the generation valid=false")

    -- annotations discovered without executing app source
    ok(disc.indexes.by_annotation["route"], "@route discovered in app.lua")
    ok(disc.indexes.by_annotation["resource"], "@resource discovered in routes/users.lua")
    -- client.js contributed NO declarations (never parsed as Lua)
    ok(disc._by_source["client.js"] == nil or #disc._by_source["client.js"] == 0,
        "unsupported JS contributes zero declarations")
    eq(disc.source, "standalone", "standalone provenance marker")
    eq(disc.generation, 0, "standalone generation is 0")
end

-- ── a genuinely clean all-Lua project -> valid + complete ───────────
do
    local analyze = require("hull.project.analyze")
    _G.tool = make_tool({
        ["ok/app.lua"] = "---@main\nlocal function main() end\nreturn main\n",
        ["ok/lib/util.lua"] = "---@util\nlocal function u() end\nreturn u\n",
    })
    local disc = analyze.analyze("ok")
    _G.tool = nil
    ok(disc.valid and disc.complete, "clean all-Lua project -> valid + complete")
    eq(disc.summary.sources_analyzed, 2, "both Lua sources analyzed")
    eq(disc.summary.sources_unsupported, 0, "no unsupported sources")
end

-- ── determinism: two runs, identical declaration id ordering ────────
do
    local analyze = require("hull.project.analyze")
    local FS = { ["p/a.lua"] = "---@x\nlocal a=1\n---@y\nlocal b=2\nreturn a,b\n",
                 ["p/b.lua"] = "---@z\nlocal c=3\nreturn c\n" }
    _G.tool = make_tool(FS); local d1 = analyze.analyze("p"); _G.tool = nil
    _G.tool = make_tool(FS); local d2 = analyze.analyze("p"); _G.tool = nil
    local ids1, ids2 = {}, {}
    for _, x in ipairs(d1.declarations) do ids1[#ids1 + 1] = x.id end
    for _, x in ipairs(d2.declarations) do ids2[#ids2 + 1] = x.id end
    eq(table.concat(ids1, "|"), table.concat(ids2, "|"), "analyze() is deterministic across runs")
end

-- ── root validation: a missing / non-directory root is NOT a clean empty project ──
do
    local analyze = require("hull.project.analyze")
    _G.tool = make_tool({ ["real/app.lua"] = "return 1\n" })
    local missing = analyze.analyze("nope")        -- root does not exist
    local afile   = analyze.analyze("real/app.lua") -- root is a FILE, not a dir
    _G.tool = nil
    ok(not missing.valid and not missing.complete, "missing root -> invalid + incomplete")
    ok(#missing.sources == 0, "missing root -> no sources (never a valid empty scan)")
    local found = false
    for _, d in ipairs(missing.diagnostics) do if d.code == "project.discovery_failed" then found = true end end
    ok(found, "missing root emits project.discovery_failed")
    ok(not afile.valid, "a file (non-directory) root is invalid too")
end

-- ── protected boundary: an internal frontend failure -> project.internal, not a raise ──
do
    local analyze = require("hull.project.analyze")
    local orig = frontend.declarations
    frontend.declarations = function() error("injected frontend defect") end   -- luacheck: ignore
    _G.tool = make_tool({ ["x/app.lua"] = "local a = 1\nreturn a\n" })
    local ok_call, disc = pcall(analyze.analyze, "x")     -- must NOT raise
    _G.tool = nil
    frontend.declarations = orig
    ok(ok_call, "analyze never raises on an internal frontend defect")
    ok(disc and not disc.valid, "internal defect -> invalid discovery")
    local found = false
    for _, d in ipairs(disc.diagnostics or {}) do if d.code == "project.internal" then found = true end end
    ok(found, "internal defect surfaces a structured project.internal diagnostic")
end

-- ── handles: generation-unique across sources + resolvable to {frontend, unit, decl} ──
do
    local analyze = require("hull.project.analyze")
    _G.tool = make_tool({
        ["h/a.lua"] = "local a = 1\nlocal b = 2\nreturn a, b\n",   -- 2 decls
        ["h/b.lua"] = "local c = 3\nreturn c\n",                    -- 1 decl
    })
    local disc = analyze.analyze("h")
    _G.tool = nil
    -- collect every handle across both sources from the internal per-source data
    local seen, count = {}, 0
    for _, decls in pairs(disc._by_source) do
        for _, d in ipairs(decls) do
            count = count + 1
            ok(d.handle ~= nil and not seen[d.handle], "handle is generation-unique: " .. tostring(d.handle))
            seen[d.handle] = true
        end
    end
    eq(count, 3, "three declarations across two sources")
    -- resolve one handle back through the controlled lookup
    local any_handle = next(seen)
    local resolved = analyze.resolve_handle(disc, any_handle)
    ok(resolved and resolved.frontend and resolved.unit and resolved.declaration,
        "resolve_handle returns {frontend, unit, declaration}")
    ok(analyze.resolve_handle(disc, 99999) == nil, "an unknown handle resolves to nil")
end

-- ── scope capability is callable THROUGH the frontend boundary ──────
do
    local unit = frontend.parse("local x = 1\nreturn x\n", "t.lua")
    ok(type(frontend.scope) == "function", "frontend exposes a scope operation (advertised capability is callable)")
    local sc, serr = frontend.scope(unit)
    ok(sc ~= nil and serr == nil, "frontend.scope(unit) resolves a scope model")
    ok(sc.bindings ~= nil, "scope model carries bindings")
end

-- ── recovery path: a model.build defect must NOT re-raise outside the pcall ──
do
    local analyze = require("hull.project.analyze")
    local orig = model.build
    model.build = function() error("injected model.build defect") end   -- luacheck: ignore
    _G.tool = make_tool({ ["m/app.lua"] = "local a = 1\nreturn a\n" })
    -- also pass numeric opts: the recovery path must not deref a non-table either
    local ok_call, disc = pcall(analyze.analyze, "m", 5)
    _G.tool = nil
    model.build = orig
    ok(ok_call, "analyze never raises even when model.build ITSELF fails")
    ok(disc and not disc.valid and not disc.complete, "model.build defect -> invalid + incomplete")
    local found = false
    for _, d in ipairs(disc.diagnostics or {}) do if d.code == "project.internal" then found = true end end
    ok(found, "model.build defect surfaces project.internal (recovery uses the literal constructor)")
    ok(disc.schema_version == 1 and disc.indexes and disc.summary and disc._handles ~= nil,
        "the emergency model is structurally complete + consumable")
end

-- ── malformed opts: never raises, wrong-typed fields normalized to defaults ──
do
    local analyze = require("hull.project.analyze")
    _G.tool = make_tool({ ["o/app.lua"] = "local a = 1\nreturn a\n" })
    local ok1 = pcall(analyze.analyze, "o", 5)               -- numeric opts
    local ok2 = pcall(analyze.analyze, "o", "nope")          -- string opts
    local ok3, d3 = pcall(analyze.analyze, "o", { generation = "x", source_kind = 7 })
    _G.tool = nil
    ok(ok1, "analyze never raises on numeric opts")
    ok(ok2, "analyze never raises on string opts")
    ok(ok3, "analyze never raises on wrong-typed opts fields")
    ok(d3 and d3.generation == 0 and d3.source == "standalone",
        "wrong-typed opts.generation / opts.source_kind normalized to defaults")
end

-- ── shared projection: public fields kept, generation-internal state dropped ──
do
    local projection = require("hull.project.projection")
    local analyze = require("hull.project.analyze")
    _G.tool = make_tool({ ["pj/app.lua"] = "---@x\nlocal a = 1\nreturn a\n" })
    local disc = analyze.analyze("pj")
    _G.tool = nil
    -- the in-memory model carries internal state...
    ok(disc._handles ~= nil and disc._by_source ~= nil, "in-memory model retains internal state")
    ok(disc.declarations[1].handle ~= nil, "in-memory decl carries a handle")
    -- ...the projection drops ALL of it, keeps the public fields
    local p = projection.project(disc)
    ok(p._handles == nil and p._by_source == nil, "projection drops _handles / _by_source")
    ok(p.indexes.by_id == nil, "projection drops the by_id decl map")
    ok(p.declarations[1].handle == nil, "projection drops per-decl handle")
    ok(p.schema_version == 1 and p.valid ~= nil and p.complete ~= nil and p.summary ~= nil,
        "projection keeps the public wire fields")
    ok(p.declarations[1].id ~= nil and p.declarations[1].annotations[1].name == "x",
        "projection keeps public decl id + annotations")
end

-- ── build seam (D10): the abstraction is consumable by a future lowering consumer ──
-- Simulates what a Query/Compute IR lowerer does through the PUBLIC model + the frontend
-- boundary -- annotation-name -> ids -> decl -> handle -> frontend semantics -- with no
-- AST traversal and no build.lua involvement.
do
    local analyze = require("hull.project.analyze")
    _G.tool = make_tool({
        ["q/app.lua"] =
            "---@query users\nlocal function list_users() end\n" ..
            "---@compute score\nlocal function score() end\nreturn list_users, score\n",
    })
    local disc = analyze.analyze("q")   -- exactly what a build consumer would call
    _G.tool = nil

    ok(disc.valid and disc.complete, "a build-ready project analyzes valid + complete")
    -- 1. reach annotated decls by annotation NAME (no per-frontend AST walk)
    ok(disc.indexes.by_annotation["query"] and #disc.indexes.by_annotation["query"] == 1,
        "consumer finds @query-annotated decls by annotation name")
    -- 2. resolve the id to the normalized decl fact
    local qid = disc.indexes.by_annotation["query"][1]
    local qdecl = disc.indexes.by_id[qid]
    ok(qdecl and qdecl.name == "list_users" and qdecl.kind == "local_function",
        "consumer resolves the @query decl (name + kind) via by_id")
    -- 3. reach FRONTEND-specific semantics THROUGH the boundary via the handle
    local resolved = analyze.resolve_handle(disc, qdecl.handle)
    ok(resolved and resolved.frontend and resolved.unit and resolved.declaration,
        "consumer resolves the decl handle to {frontend, unit, declaration}")
    -- 4. the frontend exposes scope through the contract (consumer never touches the AST)
    local sc = resolved.frontend.scope(resolved.unit)
    ok(sc and sc.bindings, "consumer reaches scope via the frontend boundary")
    -- distinct domains stay separable for distinct lowerers
    ok(disc.indexes.by_annotation["compute"], "a @compute lowerer finds its own decls independently")
end

-- ── frontend semantic recovery: initializer / function semantics via resolve_handle ──
-- Simulates a future Lua-specific lowerer: find an annotated decl -> resolve its handle ->
-- ask the frontend for the declaration's SOURCE semantics (initializer expr / function
-- params+body) -- all WITHOUT the neutral model or the wire carrying any AST.
do
    local analyze    = require("hull.project.analyze")
    local projection = require("hull.project.projection")
    local json       = require("hull.json")
    _G.tool = make_tool({ ["s/app.lua"] =
        "---@query\nlocal q = orders_where()\n" ..
        "---@query\nlocal a, r, c = 41, foo(), \"z\"\n" ..            -- distinguishable kinds by index
        "---@query\nlocal first, second = query()\n" ..               -- multi-return RHS (not positional)
        "---@compute\nlocal function dot(a, b) return a + b end\n" ..
        "---@compute\nfunction pipeline.step(x) return x end\n" ..
        "---@note\nlocal u\n" ..                                       -- no initializer (legit nil)
        "return q, r, dot\n" })
    local disc = analyze.analyze("s")
    _G.tool = nil

    -- resolve an annotated decl by name to (semantics, resolved-handle)
    local function sem_of(name)
        for _, d in ipairs(disc.declarations) do
            if d.name == name then
                local res = analyze.resolve_handle(disc, d.handle)
                ok(res and res.frontend and res.unit and res.declaration,
                    "resolve_handle -> {frontend, unit, declaration}: " .. name)
                return res.frontend.declaration_semantics(res.declaration), res
            end
        end
        return nil, nil
    end

    -- 1. @query local: the initializer expression is recovered with its exact Lua kind + range
    local qsem, qres = sem_of("q")
    ok(qsem and qsem.form == "value", "@query local q -> value form")
    ok(qsem.positional_value and qsem.positional_value.kind == "call", "q initializer is a call expr (Lua kind)")
    ok(#qsem.values == 1, "q value list carries the single RHS expression")
    eq(qres.unit:text(qsem.positional_value), "orders_where()", "initializer maps to the EXACT original source text")

    -- 2. positional multi-name: each name recovers ITS OWN initializer by index (1:1 RHS)
    local asem = sem_of("a"); local rsem = sem_of("r"); local csem = sem_of("c")
    eq(asem.name_index, 1, "a is name_index 1"); ok(asem.positional_value.kind == "literal", "a initializer is a literal (41)")
    eq(rsem.name_index, 2, "r is name_index 2"); ok(rsem.positional_value.kind == "call", "r initializer is the call foo()")
    eq(csem.name_index, 3, "c is name_index 3"); ok(csem.positional_value.kind == "literal", "c initializer is a literal (\"z\")")
    ok(#asem.values == 3 and #rsem.values == 3, "each multi-name member carries the FULL RHS list")

    -- 2b. NON-positional multi-return RHS: `local first, second = query()` -- BOTH derive from
    --     query(); the full RHS list is preserved so `second` is not misrepresented as nil-sourced.
    local fsem = sem_of("first"); local ssem = sem_of("second")
    eq(fsem.name_index, 1, "first is name_index 1")
    eq(ssem.name_index, 2, "second is name_index 2")
    ok(#fsem.values == 1 and fsem.values[1].kind == "call", "RHS is a single call query() shared by both names")
    ok(#ssem.values == 1 and ssem.values[1].kind == "call", "second retains the SAME complete RHS list (query())")
    ok(fsem.positional_value and fsem.positional_value.kind == "call", "first's positional value is query()")
    ok(ssem.positional_value == nil, "second has no POSITIONAL value (it is the 2nd return of query()), but the RHS is retained")

    -- 3. @compute local function: params + body recovered
    local dsem = sem_of("dot")
    ok(dsem and dsem.form == "function", "@compute local function dot -> function form")
    ok(#dsem.params == 2 and dsem.params[1].name == "a" and dsem.params[2].name == "b", "dot params recovered (a, b)")
    ok(type(dsem.body) == "table" and #dsem.body >= 1, "dot function body available to the frontend")
    ok(not dsem.is_method and not dsem.is_vararg, "dot is a plain (non-method, non-vararg) function")

    -- 4. @compute global (dotted) function
    local psem = sem_of("pipeline.step")
    ok(psem and psem.form == "function" and #psem.params == 1 and psem.params[1].name == "x",
        "global function pipeline.step semantics recovered (param x)")

    -- 5. a legitimate no-initializer local -> empty RHS + nil positional value, NO error (a
    --    non-nil `usem` proves no error was returned: a corrupt state returns nil, err instead).
    local usem = sem_of("u")
    ok(usem and usem.form == "value" and usem.positional_value == nil and #usem.values == 0,
        "local with no initializer -> empty values + nil positional (legit, not an error)")

    -- annotations still attach to each name of the multi-name group (unchanged semantics)
    for _, nm in ipairs({ "a", "r", "c" }) do
        local d
        for _, x in ipairs(disc.declarations) do if x.name == nm then d = x end end
        ok(d and d.annotations[1] and d.annotations[1].name == "query",
            "@query still attached to multi-name member: " .. nm)
    end

    -- 6. impossible/corrupt frontend state -> STRUCTURED error, never a silent nil or a
    --    plausible-looking record. Every retained-declaration invariant is validated.
    local function corrupt(d, why)
        local bad, berr = frontend.declaration_semantics(d)
        ok(bad == nil and berr and berr.severity == "error" and berr.code == "lua.internal",
            "corrupt decl -> structured lua.internal diagnostic: " .. why)
    end
    -- a well-formed 1-name local_declaration node the tests perturb one field at a time
    local function lnode(names, values) return { kind = "local_declaration", names = names, values = values } end
    corrupt({ _kind = "local" }, "missing _node")
    corrupt({ _kind = "local", _name = "q", _node = { kind = "function_declaration" }, _name_index = 1 },
        "local decl but node kind is function_declaration (mismatch)")
    corrupt({ _kind = "local", _name = "q", _node = { kind = "local_declaration", values = {} }, _name_index = 1 },
        "names not a list")
    corrupt({ _kind = "local", _name = "q", _node = lnode({ { name = "q" } }, {}) }, "missing name index")
    corrupt({ _kind = "local", _name = "q", _node = lnode({ { name = "q" } }, {}), _name_index = "x" },
        "non-integer name index")
    corrupt({ _kind = "local", _name = "q", _node = lnode({ { name = "q" } }, {}), _name_index = 0 },
        "name index < 1")
    corrupt({ _kind = "local", _name = "q", _node = lnode({ { name = "q" } }, {}), _name_index = 999 },
        "name index OUT OF RANGE (past #names)")
    corrupt({ _kind = "local", _name = "q", _node = lnode({ { name = "other" } }, {}), _name_index = 1 },
        "name index identifies a DIFFERENT name than recorded")
    corrupt({ _kind = "local", _name = "v", _node = lnode({ { name = "v" } }, "oops"), _name_index = 1 },
        "malformed values (not a list)")
    corrupt({ _kind = "function", _node = { kind = "local_declaration", params = {}, body = {} } },
        "function decl but node kind is local_declaration (mismatch)")
    corrupt({ _kind = "function", _node = { kind = "function_declaration", params = "x", body = {} } },
        "malformed function params (not a list)")
    corrupt({ _kind = "function", _node = { kind = "function_declaration", params = {}, body = 7 } },
        "malformed function body (not a list)")

    -- 7. the semantics + AST stay PRIVATE: the public projection carries no frontend node
    local p = projection.project(disc)
    local leaked = false
    for _, d in ipairs(p.declarations) do
        if d._node ~= nil or d._name_index ~= nil then leaked = true end
    end
    ok(not leaked, "public projection declarations carry no frontend AST node / private index")
    local blob = json.encode(p)
    ok(not blob:find("_node", 1, true) and not blob:find("local_declaration", 1, true)
        and not blob:find("function_expr", 1, true) and not blob:find("method_call", 1, true)
        and not blob:find("orders_where", 1, true),
        "serialized projection contains NO raw AST markers or initializer source")

    -- 8. handles are generation-local: out-of-range -> nil; a fresh analysis has its own table
    local hmax = 0
    for _, d in ipairs(disc.declarations) do if d.handle > hmax then hmax = d.handle end end
    ok(analyze.resolve_handle(disc, hmax + 1000) == nil, "an out-of-range handle -> nil (generation-local)")
    _G.tool = make_tool({ ["s/app.lua"] = "---@query\nlocal q = foo()\nreturn q\n" })
    local discB = analyze.analyze("s"); _G.tool = nil
    ok(disc._handles ~= discB._handles, "each generation owns a distinct handle table (no cross-generation identity)")
end

-- ── symmetric semantic-handle lifecycle through the analyzer APIs ──
-- The JS bridge (tool.frontend_*) is MOCKED here (the real QuickJS manager is C-tested); this
-- proves the analyzer's lease + analyze.declaration_semantics / analyze.scope / analyze.close gate
-- Lua and JavaScript IDENTICALLY, through the public APIs (never proxy internals).
do
    local analyze    = require("hull.project.analyze")
    local projection = require("hull.project.projection")
    local json       = require("hull.json")

    -- A tool stub over a mixed FS with a MOCK JS frontend bridge. Tracks open/close.
    local function make_mixed_tool(fs, track)
        local t = make_tool(fs)
        t.frontend_available = function(lang) return lang == "javascript" end
        t.frontend_open = function(lang) track.opened = (track.opened or 0) + 1; return 1 end
        t.frontend_close = function(lang, token) track.closed = (track.closed or 0) + 1 end
        t.frontend_analyze = function(lang, token, path, src)
            return json.encode({ schema_version = 1, status = "analyzed", unit_id = 1, diagnostics = {},
                declarations = { {
                    kind = "function", name = "jsfn",
                    range = { start = 1, stop = 5, line = 1, col = 1 },
                    group_range = { start = 1, stop = 10, line = 1, col = 1 },
                    is_method = false, decl_id = 1,
                    annotations = { { name = "route", raw = "@route", range = { start = 1, stop = 7, line = 1, col = 1 } } },
                } } })
        end
        t.frontend_declaration_semantics = function(lang, token, id)
            return json.encode({ form = "function", is_async = false, params = {}, body = { type = "BlockStatement" } })
        end
        t.frontend_scope = function(lang, token, uid)
            return json.encode({ ok = true, bindings = {}, refs = {} })
        end
        return t
    end
    local FS = {
        ["m/app.js"]  = "/** @route */\nexport function jsfn() { return 1; }\n",
        ["m/lib.lua"] = "---@query\nlocal function q() return 1 end\nreturn q\n",
    }
    -- Find a declaration's handle by language (annotated public decls carry both).
    local function handle_for(disc, lang)
        for _, d in ipairs(disc.declarations) do if d.language == lang then return d.handle end end
        return nil
    end

    -- 1+2. DEFAULT discovery: metadata available for both; semantics rejected NOT-retained (same code).
    local trk = {}
    _G.tool = make_mixed_tool(FS, trk)
    local dflt = analyze.analyze("m")
    _G.tool = nil
    eq(#dflt.sources, 2, "default: both sources present")
    local lua_h = handle_for(dflt, "lua")
    local js_h  = handle_for(dflt, "javascript")
    ok(lua_h ~= nil and js_h ~= nil, "default: both an annotated Lua and JS handle exist")
    local _, l_err = analyze.declaration_semantics(dflt, lua_h)
    local _, j_err = analyze.declaration_semantics(dflt, js_h)
    eq(l_err and l_err.code, "project.frontend_not_retained", "default LUA semantics rejected not-retained")
    eq(j_err and j_err.code, "project.frontend_not_retained", "default JS semantics rejected (SAME code)")
    ok((trk.closed or 0) >= 1, "default: the JS session was closed after build (no leak)")

    -- 3. RETAINED mixed discovery: Lua + JS semantics + scope resolve.
    local trk2 = {}
    _G.tool = make_mixed_tool(FS, trk2)
    local ret = analyze.analyze("m", { retain_frontend = true })
    local lh, jh = handle_for(ret, "lua"), handle_for(ret, "javascript")
    local lrec, le = analyze.declaration_semantics(ret, lh)
    local jrec, je = analyze.declaration_semantics(ret, jh)
    ok(lrec ~= nil and le == nil, "retained: LUA declaration_semantics resolves")
    ok(jrec ~= nil and je == nil, "retained: JS declaration_semantics resolves")
    local lsc = analyze.scope(ret, lh)
    local jsc = analyze.scope(ret, jh)
    ok(lsc ~= nil, "retained: LUA scope resolves")
    ok(jsc ~= nil, "retained: JS scope resolves")

    -- 5. Invalid handle through the analyzer boundary: same code regardless of language.
    local _, ih = analyze.declaration_semantics(ret, 999999)
    eq(ih and ih.code, "project.handle_invalid", "invalid handle -> project.handle_invalid")

    -- 4. After analyze.close: Lua + JS semantics + scope all rejected; repeat close is safe.
    analyze.close(ret)
    _G.tool = nil
    ok((trk2.closed or 0) >= 1, "retained: analyze.close closed the JS session")
    local _, lc = analyze.declaration_semantics(ret, lh)
    local _, jc = analyze.declaration_semantics(ret, jh)
    local _, sc = analyze.scope(ret, jh)
    eq(lc and lc.code, "project.frontend_closed", "post-close LUA semantics rejected")
    eq(jc and jc.code, "project.frontend_closed", "post-close JS semantics rejected (SAME code)")
    eq(sc and sc.code, "project.frontend_closed", "post-close scope rejected")
    local ok_close2 = pcall(analyze.close, ret)
    ok(ok_close2, "repeated analyze.close is a safe no-op")

    -- 6. Public projection exposes no lease/handles/tokens/ids/AST.
    _G.tool = make_mixed_tool(FS, {})
    local pdisc = analyze.analyze("m", { retain_frontend = true })
    _G.tool = nil
    local blob = json.encode(projection.project(pdisc))
    local leaked = false
    for _, bad in ipairs({ "_frontend_lease", "_handles", "_by_source", "session", "decl_id",
                           "unit_id", "_session", "_unit_id", "_decl_id", "BlockStatement" }) do
        if blob:find(bad, 1, true) then leaked = true; break end
    end
    ok(not leaked, "public projection exposes no lease / handle / token / unit_id / decl_id / AST")
    analyze.close(pdisc)

    -- 7. Failure cleanup: a mid-analysis fault closes the JS session AND leaves the discovery not
    -- semantically live (rejected through the analyzer API, same as any non-retained discovery).
    local trk3 = {}
    _G.tool = make_mixed_tool(FS, trk3)
    _G.tool.frontend_analyze = function() error("injected mid-analysis fault") end   -- force a raise
    local okf, dfail = pcall(analyze.analyze, "m", { retain_frontend = true })
    _G.tool = nil
    ok(okf, "a mid-analysis fault does not raise out of analyze (protected boundary)")
    ok((trk3.closed or 0) >= 1, "failure cleanup: the JS session was closed on the fault path")
    local _, fe2 = analyze.declaration_semantics(dfail, 1)
    ok(fe2 ~= nil and (fe2.code == "project.frontend_not_retained" or fe2.code == "project.handle_invalid"),
        "a failed discovery is not semantically live")
end

-- ── TypeScript: the mode reaches the bridge, type_erasure is a gated unit operation ──
-- The bridge is MOCKED (test_js_generation covers the real session); this proves the analyzer
-- passes lang "ts" for .ts / .mts and "js" for .js, keys type_erasure by source path, reports
-- it unsupported for other frontends, and gates it by the same lease as the other operations.
do
    local analyze = require("hull.project.analyze")
    local json    = require("hull.json")
    local function make_ts_tool(fs, track)
        local t = make_tool(fs)
        t.frontend_available = function(lang) return lang == "javascript" end
        t.frontend_open = function() return 7 end
        t.frontend_close = function() track.closed = (track.closed or 0) + 1 end
        local next_unit = 0
        t.frontend_analyze = function(lang, token, path, src, mode)
            next_unit = next_unit + 1
            track.modes = track.modes or {}
            track.modes[path] = mode
            track.units = track.units or {}
            track.units[next_unit] = path
            return json.encode({ schema_version = 1, status = "analyzed", unit_id = next_unit,
                                 diagnostics = {}, declarations = {} })
        end
        t.frontend_type_erasure = function(lang, token, unit_id)
            track.erased = track.units[unit_id]
            return json.encode({ ok = true, spans = { { start = 8, stop = 16 } },
                refusals = { { severity = "error", code = "ts.unsupported", message = "m",
                               range = { start = 1, stop = 5, line = 1, col = 1 } } } })
        end
        return t
    end
    local FS = {
        ["t/app.ts"]    = "const x: number = 1;\n",
        ["t/lib/a.mts"] = "export const y = 1;\n",
        ["t/old.js"]    = "export const z = 1;\n",
        ["t/q.lua"]     = "local q = 1\nreturn q\n",
        ["t/v.tsx"]     = "export const v = <div/>;\n",
    }

    local trk = {}
    _G.tool = make_ts_tool(FS, trk)
    local disc = analyze.analyze("t", { retain_frontend = true })
    eq(trk.modes["app.ts"], "ts", ".ts analyzed in TypeScript mode")
    eq(trk.modes["lib/a.mts"], "ts", ".mts analyzed in TypeScript mode")
    eq(trk.modes["old.js"], "js", ".js analyzed in JavaScript mode")
    local status = {}
    for _, s in ipairs(disc.sources) do status[s.path] = s.language .. ":" .. s.status end
    eq(status["app.ts"], "typescript:analyzed", "app.ts is an analyzed typescript source")
    eq(status["v.tsx"], "typescript:unsupported", ".tsx is an unsupported typescript source")
    ok(not disc.complete, "a .tsx source makes the generation complete=false")
    local tsx_msg
    for _, d in ipairs(disc.diagnostics) do if d.path == "v.tsx" then tsx_msg = d.message end end
    ok(tsx_msg and tsx_msg:find("TSX", 1, true), "the .tsx diagnostic names TSX")

    local res, err = analyze.type_erasure(disc, "app.ts")
    ok(res ~= nil and err == nil, "type_erasure resolves for a .ts source that declares nothing")
    eq(trk.erased, "app.ts", "type_erasure reached app.ts's own unit")
    eq(res and res.spans[1].start, 8, "spans come back from the bridge")
    eq(res and res.refusals[1].code, "ts.unsupported", "refusals come back from the bridge")
    local _, e_js = analyze.type_erasure(disc, "old.js")
    eq(e_js and e_js.code, "project.operation_unsupported", "a .js source reports type_erasure unsupported")
    local _, e_lua = analyze.type_erasure(disc, "q.lua")
    eq(e_lua and e_lua.code, "project.operation_unsupported", "a .lua source reports type_erasure unsupported")
    local _, e_none = analyze.type_erasure(disc, "v.tsx")
    eq(e_none and e_none.code, "project.handle_invalid", "an unanalyzed source has no unit")
    local _, e_bad = analyze.type_erasure(disc, 1)
    eq(e_bad and e_bad.code, "project.handle_invalid", "a non-path key does not resolve")
    analyze.close(disc)
    local _, e_closed = analyze.type_erasure(disc, "app.ts")
    eq(e_closed and e_closed.code, "project.frontend_closed", "type_erasure is refused after close")

    local dflt = analyze.analyze("t")
    _G.tool = nil
    local _, e_nr = analyze.type_erasure(dflt, "app.ts")
    eq(e_nr and e_nr.code, "project.frontend_not_retained", "type_erasure needs a retained discovery")
    local blob = json.encode(require("hull.project.projection").project(dflt))
    ok(not blob:find("_units", 1, true), "projection drops the per-source unit table")
end

print(string.format("test_project: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
