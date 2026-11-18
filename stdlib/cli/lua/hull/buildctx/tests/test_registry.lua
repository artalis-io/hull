-- luacheck: globals T
-- Build plugin registration (docs/buildcontext_design.md §9.2):
-- hull.build.json, plugin directories and identities, the manifest preset
-- (the runner's own check, through hull.buildctx._plugin), descriptors,
-- configuration and claims. Manifests come from a table here (the tool VM
-- extracts them with the manifest-extraction child). Run by
-- tests/hull/buildctx/test_buildctx.c, which provides T.

local registry = require("hull.buildctx.registry")
local json = require("hull.json")

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
local root = T.tmpdir("registry")
roots[#roots + 1] = root
local function put(rel, data)
    local dir = rel:match("^(.*)/[^/]+$")
    if dir then
        local p = root
        for part in dir:gmatch("[^/]+") do p = p .. "/" .. part; T.mkdir(p) end
    end
    local f = assert(io.open(root .. "/" .. rel, "wb")); f:write(data); f:close()
end

local DESC = { id = "demo", version = 1, input = "source", languages = { "lua" },
               output = "generated", config = { dialect = { type = "string", required = true } } }

-- entry (absolute) -> the manifest JSON its extraction returns
local manifests = {}
local function manifest_for(dir, m) manifests[root .. "/" .. dir] = json.encode(m) end
local function extract(entry)
    local m = manifests[entry:gsub("/app%.[a-z]+$", "")]
    if not m then return nil, "no manifest" end
    return m
end
local function load() return registry.load(root, { extract = extract, sha256_hex = T.sha256 }) end

-- ── no hull.build.json: only the bundled list ────────────────────────
do
    local reg = assert(load())
    eq(#reg.plugins, 0, "no file, no application plugins")
    eq(reg.config_sha256, nil, "and no config hash")
end

-- ── two application plugins ──────────────────────────────────────────
put("plugins/a/app.lua", "-- a\n")
put("plugins/a/lib/util.lua", "return 1\n")
put("plugins/b/app.js", "// b\n")
manifest_for("plugins/a", { modules = { "hull/build-plugin@1", "hull/json@1" }, build_plugin = DESC })
manifest_for("plugins/b", { modules = { "hull/build-plugin@1" }, buildPlugin = {
    id = "art", version = 2, input = "artifact", kinds = { "demo-tree" }, output = "bundle" } })
local function config(t) put("hull.build.json", json.encode(t)) end
config({ plugins = { { path = "plugins/a", config = { dialect = "pg" } }, { path = "plugins/b" } } })
local id_a
do
    local reg, err = load()
    ok(reg, "two plugins register (" .. tostring(err) .. ")")
    if reg then
        eq(#reg.plugins, 2, "two plugins")
        local a, b = reg.plugins[1], reg.plugins[2]
        ok(a.identity:match("^app:plugins/a#%x+$") and #a.identity == #"app:plugins/a#" + 64,
           "an application identity: app:<path>#<sha256>")
        eq(a.language, "lua", "app.lua is a Lua plugin")
        eq(b.language, "javascript", "app.js is a JavaScript plugin")
        eq(a.entry, root .. "/plugins/a/app.lua", "the absolute entry")
        eq(a.config.dialect, "pg", "its configuration")
        eq(reg:route("source:lua:*"), a, "a source plugin with no annotations claims every unit")
        eq(reg:route("artifact:demo-tree"), b, "an artifact plugin claims its kinds")
        eq(#reg.config_sha256, 64, "the config file's hash")
        id_a = a.identity
    end
end
put("plugins/a/lib/util.lua", "return 2\n")
do
    local reg = assert(load())
    ok(reg.plugins[1].identity ~= id_a, "editing any file of a plugin changes its identity")
end

-- ── hull.build.json, strictly ────────────────────────────────────────
local function refused(cfg, needle, name)
    if type(cfg) == "string" then put("hull.build.json", cfg) else config(cfg) end
    local reg, err = load()
    ok(reg == nil, name .. " is refused")
    has(err, needle, name)
end
refused("[1]", "must be a JSON object", "a top-level array")
refused("{", "must be a JSON object", "malformed JSON")
refused({ plugins = {}, extra = 1 }, "unknown key 'extra'", "an unknown top-level key")
refused({ plugins = { { path = "plugins/a", other = 1 } } }, "unknown key 'other'", "an unknown entry key")
refused({ plugins = { { path = "../x" } } }, "relative path inside the app", "a path out of the app")
refused({ plugins = { { path = "/abs" } } }, "relative path inside the app", "an absolute path")
refused({ plugins = { { path = "plugins/a/" } } }, "relative path inside the app", "a trailing slash")
refused({ plugins = { { path = ".hull/p" } } }, "inside .hull", "a path into .hull")
refused({ plugins = { { path = ".HULL" } } }, "inside .hull", ".hull in any case")
refused({ plugins = { { path = "plugins/a", config = { dialect = "pg" } }, { path = "plugins/a" } } },
        "named twice", "a plugin named twice")
refused({ plugins = { { path = "plugins/none" } } }, "exactly one of app.lua / app.js", "no entry")
put("plugins/both/app.lua", "")
put("plugins/both/app.js", "")
refused({ plugins = { { path = "plugins/both" } } }, "exactly one of app.lua / app.js", "two entries")

-- ── configuration against the schema ─────────────────────────────────
refused({ plugins = { { path = "plugins/a" } } }, "config 'dialect' is required", "a required key missing")
refused({ plugins = { { path = "plugins/a", config = { dialect = 1 } } } }, "must be string", "a wrong type")
refused({ plugins = { { path = "plugins/a", config = { dialect = "pg", x = true } } } },
        "'x' is not in the plugin's schema", "a key outside the schema")

-- ── links ────────────────────────────────────────────────────────────
put("plugins/l/app.lua", "")
manifest_for("plugins/l", { modules = { "hull/build-plugin@1" }, build_plugin = DESC })
T.symlink(root .. "/plugins/a/app.lua", root .. "/plugins/l/linked.lua")
refused({ plugins = { { path = "plugins/l", config = { dialect = "pg" } } } }, "a symlink", "a link inside a plugin")
T.symlink(root .. "/plugins/a", root .. "/plugins/via")
refused({ plugins = { { path = "plugins/via", config = { dialect = "pg" } } } }, "plugins/via",
        "a plugin directory that is a link")
put("real.json", json.encode({ plugins = {} }))
os.remove(root .. "/hull.build.json")
T.symlink(root .. "/real.json", root .. "/hull.build.json")
do
    local reg, err = load()
    ok(reg == nil, "a linked hull.build.json is refused")
    has(err, "hull.build.json", "the linked config")
end
os.remove(root .. "/hull.build.json")

-- ── the manifest preset and the descriptor ───────────────────────────
put("plugins/p/app.lua", "")
local function plugin_manifest(m, needle, name)
    manifest_for("plugins/p", m)
    refused({ plugins = { { path = "plugins/p" } } }, needle, name)
end
plugin_manifest({ modules = { "hull/build-plugin@1" }, fs = { read = { "x" } }, build_plugin = DESC },
                "may not declare `fs`", "a plugin declaring fs")
plugin_manifest({ modules = { "hull/build-plugin@1", "hull/time@1" }, build_plugin = DESC },
                "hull/time", "a plugin declaring a time module")
plugin_manifest({ modules = { "hull/json@1" }, build_plugin = DESC }, "hull/build-plugin@1",
                "a manifest without hull/build-plugin")
plugin_manifest({ modules = { "hull/build-plugin@1" } }, "no build_plugin descriptor", "no descriptor")
local function with(k, v)
    local d = {}
    for kk, vv in pairs(DESC) do d[kk] = vv end
    d[k] = v
    return { modules = { "hull/build-plugin@1" }, build_plugin = d }
end
plugin_manifest(with("extra", 1), "unknown descriptor key 'extra'", "an unknown descriptor key")
plugin_manifest(with("id", "Bad_Id"), "descriptor id", "a bad id")
plugin_manifest(with("version", 0), "descriptor version", "version 0")
plugin_manifest(with("input", "stdin"), "descriptor input", "an unknown input class")
plugin_manifest(with("languages", { "cobol" }), "descriptor languages", "an unknown language")
plugin_manifest(with("kinds", { "x" }), "takes no kinds", "kinds on a source plugin")
plugin_manifest(with("config", { x = { type = "float" } }), "descriptor config 'x'", "an unknown config type")
manifests[root .. "/plugins/p"] = nil
refused({ plugins = { { path = "plugins/p" } } }, "could not be read", "a plugin whose manifest cannot be read")

-- ── claims ───────────────────────────────────────────────────────────
put("plugins/c/app.lua", "")
manifest_for("plugins/c", { modules = { "hull/build-plugin@1" }, build_plugin = {
    id = "other", version = 1, input = "source", languages = { "javascript", "lua" },
    output = "generated" } })
refused({ plugins = { { path = "plugins/a", config = { dialect = "pg" } }, { path = "plugins/c" } } },
        "both claim source:lua:*", "two plugins claiming one kind")
do
    put("hull.build.json", json.encode({ plugins = { { path = "plugins/c" } } }))
    local reg, err = registry.load(root, {
        extract = extract, sha256_hex = T.sha256,
        bundled = { { id = "ts-erase", major = 1, descriptor = {
            id = "ts-erase", version = 1, input = "source", languages = { "typescript" },
            output = "javascript" } } },
    })
    ok(reg, "a bundled plugin and an application plugin (" .. tostring(err) .. ")")
    if reg then
        eq(reg.plugins[1].identity, "stdlib:ts-erase@1", "a bundled identity: stdlib:<id>@<major>")
        eq(reg:route("source:typescript:*").identity, "stdlib:ts-erase@1", "the bundled route")
    end
    reg, err = registry.load(root, {
        extract = extract, sha256_hex = T.sha256,
        bundled = { { id = "js", major = 1, descriptor = {
            id = "js", version = 1, input = "source", languages = { "javascript" }, output = "x" } } },
    })
    ok(reg == nil, "an application plugin cannot take a bundled plugin's kind")
    has(err, "both claim source:javascript:*", "the bundled conflict")
end

for _, r in ipairs(roots) do T.rmtree(r) end
return { pass = pass, fail = fail }
