--
-- test_project.lua — Slice 1 tests for the project source-discovery layer.
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
    eq(table.concat(exts, ","), "cjs,js,lua,mjs", "known_exts sorted + reserves JS variants")
    ok(registry.for_ext("lua").analyzable, "lua is analyzable")
    ok(not registry.for_ext("js").analyzable, "js is known but NOT analyzable")
    ok(registry.for_ext("py") == nil, "unknown extension is not registered")
    local fr = registry.frontends()
    local lua_e, js_e
    for _, e in ipairs(fr) do
        if e.language == "lua" then lua_e = e elseif e.language == "javascript" then js_e = e end
    end
    ok(lua_e and lua_e.analyzable and #lua_e.capabilities == 4, "lua frontend reports its 4 shipped capabilities")
    ok(js_e and not js_e.analyzable and #js_e.capabilities == 0, "javascript reserved: analyzable=false, no capabilities")
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

print(string.format("test_project: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
