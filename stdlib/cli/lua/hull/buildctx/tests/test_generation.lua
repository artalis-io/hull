-- luacheck: globals T
-- A pipeline generation from pipeline results (hull.buildctx.generation,
-- docs/dev_pipeline_generations_design.md §1.2): generated outputs become
-- entries named for the app files they shadow, artifacts do not, the inputs
-- are the observed paths, and the bundle is what an app run from source
-- takes. Run by tests/hull/buildctx/test_buildctx.c, which provides T.

local gen = require("hull.buildctx.generation")
local json = require("hull.json")

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1 else fail = fail + 1; io.stderr:write("  FAIL: " .. name .. "\n") end
end
local function eq(a, b, name)
    ok(a == b, name .. " (got " .. tostring(a) .. ", want " .. tostring(b) .. ")")
end

local sha = T.sha256

local function deps(paths)
    local obs = {}
    for i, p in ipairs(paths) do
        obs[i] = p == "." and { op = "list", path = p, sha256 = sha(p), entries = 1 }
                         or { op = "read", path = p, sha256 = sha(p), size = #p }
    end
    return json.encode({ format = "hull-build-deps", version = 1, grants = { "src/" },
                         observations = obs })
end

local function committed(identity, outs, observed, status)
    return { identity = identity, status = status or "committed", key = sha(identity),
             diagnostics = { { severity = "info", code = "x.y", message = "m" } },
             result = { outputs = outs, deps = deps(observed or { "src/a.ts" }) } }
end

-- Parse a bundle the way the app process does: header, manifest, payloads.
local function parse(b)
    local len, rest = b:match("^hull%-generation 1 (%d+)\n()")
    if not len then return nil end
    len = tonumber(len)
    local man = json.decode(b:sub(rest, rest + len - 1))
    return man, b:sub(rest + len)
end

-- Generated outputs become entries; artifacts do not.
local results = {
    committed("p1", {
        { declared = "generated", descriptor = { name = "./src/a.ts", position_map = "identity" },
          bytes = "let x        = 1;\n" },
        { declared = "artifact", descriptor = { name = "inv", kind = "k" }, bytes = "artifact" },
    }, { "src/a.ts", "src/b.ts" }),
    committed("p2", {
        { declared = "generated", descriptor = { name = "gen" },
          entries = { { path = "m.json", mode = "regular", bytes = "{}" },
                      { path = "z/n.json", mode = "regular", bytes = "[]" } } },
    }, { "src/a.ts", "." }, "cached"),
    { identity = "p3", status = "no_input" },
    { identity = "p4", status = "skipped", detail = "bundled" },
}
local g, err = gen.from_results(results, sha)
ok(g ~= nil, "from_results: " .. tostring(err))
eq(#g.entries, 3, "three entries")
eq(g.entries[1].name, "gen/m.json", "a tree's files are named <output>/<path>")
eq(g.entries[2].name, "gen/z/n.json", "nested tree path")
eq(g.entries[3].name, "src/a.ts", "a generated name loses its ./")
eq(g.entries[3].position_map, "identity", "position_map carried")
eq(g.entries[1].position_map, "none", "position_map defaults to none")
eq(g.entries[3].origin.plugin, "p1", "origin names the plugin")
eq(table.concat(g.inputs, ","), ".,src/a.ts,src/b.ts", "inputs: observed paths, once, sorted")
eq(g.summary.info, 2, "diagnostics summarised")

local b = assert(gen.encode(g, sha))
local man, payloads = parse(b)
ok(man ~= nil, "bundle header")
eq(man.format, "hull-pipeline-generation", "format")
eq(man.version, 1, "version")
eq(#man.entries, 3, "manifest entries")
eq(man.entries[3].vfs_name, "src/a.ts", "vfs_name")
eq(man.entries[3].content_hash, sha("let x        = 1;\n"), "content_hash")
eq(payloads, "{}" .. "[]" .. "let x        = 1;\n", "payloads follow, in entry order")
eq(man.generation_id, man.pipeline_plan_hash, "the id is the plan hash")

-- The plan hash follows the actions' keys; it is stable for the same results.
local g2 = assert(gen.from_results(results, sha))
eq(g2.plan_hash, g.plan_hash, "plan hash is deterministic")
results[1].key = sha("other")
eq(assert(gen.from_results(results, sha)).plan_hash ~= g.plan_hash, true, "a new key, a new plan")

-- An empty generation still has array members.
local e = assert(gen.from_results({ { identity = "p", status = "no_input" } }, sha))
local eb = assert(gen.encode(e, sha))
ok(eb:find('"entries":[]', 1, true) ~= nil, "empty entries is an array")
ok(eb:find('"inputs":[]', 1, true) ~= nil, "empty inputs is an array")

-- Refusals.
local r, rerr = gen.from_results({ { identity = "bad", status = "failed", error = "plugin_crashed" } }, sha)
ok(r == nil and rerr:find("plugin_crashed", 1, true), "a failed plugin fails the generation")
r, rerr = gen.from_results({
    committed("p1", { { declared = "generated", descriptor = { name = "a.lua" }, bytes = "1" } }),
    committed("p2", { { declared = "generated", descriptor = { name = "./a.lua" }, bytes = "2" } }),
}, sha)
ok(r == nil and rerr:find("both p1 and p2", 1, true), "two plugins generating one name")
for _, bad in ipairs({ ".hull/x", ".HULL/x", "hull.web.x", "hull:x", "a\\b", "c:d", "a/../b", "" }) do
    r = gen.from_results({ committed("p", { { declared = "generated", descriptor = { name = bad },
                                               bytes = "x" } }) }, sha)
    ok(r == nil, "refused name " .. bad)
end
r, rerr = gen.from_results({ committed("p", { { declared = "generated",
    descriptor = { name = "a.lua", position_map = "table" }, bytes = "x" } }) }, sha)
ok(r == nil and rerr:find("position_map", 1, true), "an unknown position_map")
r = gen.from_results({ { identity = "p", status = "committed", key = sha("p"),
                         result = { outputs = {}, deps = "not json" } } }, sha)
ok(r == nil, "a corrupt dependency manifest")

for _, good in ipairs({ "app.ts", "lib/m.lua", "compute/__generated/x.wasm", "hullx/a", ".env.lua" }) do
    ok(gen.name_ok(good), "name ok " .. good)
end

return { pass = pass, fail = fail }
