-- hull.buildctx.registry - which build plugins a pipeline run has
-- (docs/buildcontext_design.md §9.2): the bundled list, plus the application
-- plugins the app names in hull.build.json, each described by its own
-- manifest.
--
-- Host-owned tool code. Nothing here runs a plugin's transform: a plugin's
-- manifest is read with the manifest-extraction child, as `hull build` reads
-- an app's.
--
--   local registry = require("hull.buildctx.registry")
--   local reg, err = registry.load("/abs/app")
--   for _, p in ipairs(reg.plugins) do
--       -- p.identity   "stdlib:<id>@<major>" | "app:<path>#<sha256>"
--       -- p.entry      the absolute app.lua / app.js
--       -- p.claims     the input kinds it claims
--       -- p.language   "lua" | "javascript"
--       -- p.descriptor the validated build_plugin descriptor
--       -- p.config     the configuration hull.build.json gives it
--   end
--   local p = reg:route("source:typescript:*")   -- the plugin claiming a kind
--   for _, r in ipairs(reg.routes) do              -- the active Hull routes
--       -- r.id, r.unit ("module"), r.language, r.kind ("module:<language>"),
--       -- r.plugin (the bundled plugin it runs)
--   end
--
-- hull.build.json, at the app root, read without running any app code:
--   { "plugins": [ { "path": "plugins/sql_codegen", "config": { ... } } ] }
-- Strict: unknown keys are refused; `path` is canonical, inside the app tree,
-- through no symbolic link and not into .hull, and names a directory holding
-- exactly one of app.lua / app.js. Nothing else activates an application
-- plugin. Its bytes (reg.config_sha256) belong in every action's key.
--
-- The descriptor, under build_plugin (Lua) / buildPlugin (JavaScript):
--   id          [a-z][a-z0-9-]*, at most 64 bytes
--   version     an integer >= 1
--   input       "source" | "artifact"
--   languages   source: the units' languages ("lua", "javascript", "typescript")
--   annotations source, optional: the annotations it consumes (none = every unit)
--   kinds       artifact: the artifact kinds it consumes
--   output      the kind it produces
--   config      optional: { <key> = { type = "string" | "integer" | "boolean"
--                                          | "array" | "map", required = bool } }
-- Unknown keys are refused. Each application plugin claims the input kinds
-- "source:<language>:<annotation | *>" or "artifact:<kind>"; two plugins
-- claiming one kind is an error.
--
-- Routes (docs/buildcontext_design.md §13.7, typescript_design.md §4.2): a bundled
-- plugin runs only on a Hull-defined route, active whenever its input exists.
-- A route of unit "module" hands the plugin every analyzed source file of its
-- language, whole, and claims "module:<language>". An application plugin
-- cannot claim a module kind (its descriptor only names source and artifact
-- inputs), so it can neither replace nor shadow a route; a bundled plugin
-- claims its routes' kinds, never a source or artifact kind, and a bundled
-- plugin on no route, or a route naming no bundled plugin, refuses the load.
--
-- Errors are nil, message: the message names the plugin and what is wrong.

local json = require("hull.json")
local outputs = require("hull.buildctx.outputs")

local M = {}

M.CONFIG_FILE = "hull.build.json"
M.LIMITS = {
    config_bytes = 1024 * 1024,         -- hull.build.json
    plugin_files = 1024,                -- files in one plugin's directory
    plugin_bytes = 64 * 1024 * 1024,    -- their bytes, in all
    file_bytes   = 16 * 1024 * 1024,    -- one file
}

-- The plugins compiled into Hull: { id, major, descriptor, language, source }
-- - source names its embedded directory (stdlib/plugins/<source>/), whose
-- files the pipeline writes into a private directory before it runs the
-- plugin there (M.materialize). The descriptor is the one its manifest
-- declares.
M.BUNDLED = {
    {
        id = "ts-erase", major = 1, language = "javascript", source = "ts_erase",
        descriptor = { id = "ts-erase", version = 1, input = "source", languages = { "typescript" },
                       output = "generated" },
    },
}

-- The Hull-defined routes: { id, unit = "module", language, plugin } (plugin:
-- a bundled id). TypeScript erasure (typescript_design.md §4.2): every
-- analyzed TypeScript file, erased to the JavaScript an app runs.
M.ROUTES = {
    { id = "typescript-erasure", unit = "module", language = "typescript", plugin = "ts-erase" },
}
M.ROUTE_UNITS = { module = true }

-- The bundled plugins this binary embeds, each with its files and its
-- implementation hash (the hull version and every file, so a new hull or a
-- changed plugin changes every key), and the routes over them. A plugin this
-- binary does not embed is left out with its routes (a test VM embeds none).
function M.defaults(sha256_hex)
    local list, routes, have = {}, {}, {}
    local files_of = type(tool) == "table" and tool.bundled_plugin_files
    for _, b in ipairs(M.BUNDLED) do
        local files, version
        if files_of then files, version = files_of(b.source) end
        if type(files) == "table" and #files > 0 then
            local lines = { "hull-bundled-plugin 1", tostring(version) }
            for _, f in ipairs(files) do lines[#lines + 1] = f.name .. "\0" .. sha256_hex(f.body) end
            local e = {}
            for k, v in pairs(b) do e[k] = v end
            e.files, e.implementation = files, sha256_hex(table.concat(lines, "\n"))
            list[#list + 1] = e
            have[b.id] = true
        end
    end
    for _, r in ipairs(M.ROUTES) do
        if have[r.plugin] then routes[#routes + 1] = r end
    end
    return list, routes
end

-- Write a bundled plugin's embedded files into a fresh private directory
-- (tool.tmpdir) and set its entry there. Answers the directory, which the
-- caller removes, or nil, err. A plugin with an entry already needs nothing.
function M.materialize(p)
    if p.entry then return nil end
    local b = p.bundled
    local files = b and b.files
    if type(files) ~= "table" then return nil, p.identity .. ": no entry" end
    local dir = tool.tmpdir()
    if not dir then return nil, p.identity .. ": cannot make a directory for it" end
    -- A cosmo hull on Windows makes it as C:/Users/... (the form busybox
    -- takes); a plugin entry is an absolute path in cosmo's own form,
    -- /C/Users/..., which every check of a plugin path expects (#1188).
    dir = dir:gsub("^(%a):/", "/%1/")
    local entry
    for _, f in ipairs(files) do
        if type(f.name) ~= "string" or not f.name:match("^[%w_%-%.]+$") or f.name:sub(1, 1) == "." then
            tool.rmdir(dir)
            return nil, p.identity .. ": bad embedded file name"
        end
        if not tool.write_file(dir .. "/" .. f.name, f.body) then
            tool.rmdir(dir)
            return nil, p.identity .. ": cannot write " .. f.name
        end
        if f.name == "app." .. (p.language == "lua" and "lua" or "js") then entry = dir .. "/" .. f.name end
    end
    if not entry then
        tool.rmdir(dir)
        return nil, p.identity .. ": no entry"
    end
    p.entry = entry
    return dir
end

local LANGUAGES = { lua = true, javascript = true, typescript = true }
local CONFIG_TYPES = { string = true, integer = true, boolean = true, array = true, map = true }

local function is_array(t)
    if type(t) ~= "table" then return false end
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    for i = 1, n do
        if t[i] == nil then return false end
    end
    return true
end

local function only_keys(t, allowed)
    for k in pairs(t) do
        if not allowed[k] then return k end
    end
    return nil
end

local function strings(t, valid)
    if not is_array(t) then return false end
    local seen = {}
    for _, v in ipairs(t) do
        if type(v) ~= "string" or (valid and not valid(v)) or seen[v] then return false end
        seen[v] = true
    end
    return true
end

local function kind_name(s) return #s > 0 and #s <= 128 and not s:find("[^%w%-_.]") end

local DESC_KEYS = { id = true, version = true, input = true, languages = true,
                    annotations = true, kinds = true, output = true, config = true }

-- A validated copy of a descriptor, or nil, reason.
function M.check_descriptor(d)
    if type(d) ~= "table" then return nil, "no build_plugin descriptor in its manifest" end
    local extra = only_keys(d, DESC_KEYS)
    if extra then return nil, "unknown descriptor key '" .. tostring(extra) .. "'" end
    if type(d.id) ~= "string" or #d.id > 64 or not d.id:match("^[a-z][a-z0-9%-]*$") then
        return nil, "descriptor id must match [a-z][a-z0-9-]* (at most 64 bytes)"
    end
    if math.type(d.version) ~= "integer" or d.version < 1 then
        return nil, "descriptor version must be an integer >= 1"
    end
    if type(d.output) ~= "string" or not kind_name(d.output) then
        return nil, "descriptor output must name a kind"
    end
    local out = { id = d.id, version = d.version, input = d.input, output = d.output }
    if d.input == "source" then
        if d.kinds ~= nil then return nil, "a source plugin takes no kinds" end
        if not strings(d.languages, function(l) return LANGUAGES[l] end) or #d.languages == 0 then
            return nil, "descriptor languages must list lua, javascript or typescript"
        end
        if d.annotations ~= nil and not strings(d.annotations, kind_name) then
            return nil, "descriptor annotations must be a list of names"
        end
        out.languages, out.annotations = d.languages, d.annotations
    elseif d.input == "artifact" then
        if d.languages ~= nil or d.annotations ~= nil then
            return nil, "an artifact plugin takes no languages or annotations"
        end
        if not strings(d.kinds, kind_name) or #d.kinds == 0 then
            return nil, "descriptor kinds must list the artifact kinds it consumes"
        end
        out.kinds = d.kinds
    else
        return nil, "descriptor input must be \"source\" or \"artifact\""
    end
    if d.config ~= nil then
        if type(d.config) ~= "table" or (next(d.config) ~= nil and is_array(d.config)) then
            return nil, "descriptor config must map each key to its type"
        end
        out.config = {}
        for k, spec in pairs(d.config) do
            if type(k) ~= "string" or type(spec) ~= "table" or only_keys(spec, { type = true, required = true }) or
               not CONFIG_TYPES[spec.type] or (spec.required ~= nil and type(spec.required) ~= "boolean") then
                return nil, "descriptor config '" .. tostring(k) .. "' must be { type, required? }"
            end
            out.config[k] = { type = spec.type, required = spec.required == true }
        end
    end
    return out
end

-- The input kinds a descriptor claims.
function M.claims(d)
    local out = {}
    if d.input == "source" then
        for _, lang in ipairs(d.languages) do
            if d.annotations and #d.annotations > 0 then
                for _, a in ipairs(d.annotations) do out[#out + 1] = "source:" .. lang .. ":" .. a end
            else
                out[#out + 1] = "source:" .. lang .. ":*"
            end
        end
    else
        for _, k in ipairs(d.kinds) do out[#out + 1] = "artifact:" .. k end
    end
    return out
end

local function value_type(v)
    local t = type(v)
    if t == "string" or t == "boolean" then return t end
    if math.type(v) == "integer" then return "integer" end
    if t == "table" then
        if next(v) == nil then return "empty" end
        return is_array(v) and "array" or "map"
    end
    return nil
end

-- A configuration against a descriptor's schema: nil when it fits, else why.
function M.check_config(schema, config)
    if config == nil then config = {} end
    if type(config) ~= "table" or (next(config) ~= nil and is_array(config)) then
        return "config must be an object"
    end
    schema = schema or {}
    for k, v in pairs(config) do
        local spec = schema[k]
        if not spec then return "config key '" .. tostring(k) .. "' is not in the plugin's schema" end
        local vt = value_type(v)
        if vt ~= spec.type and not (vt == "empty" and (spec.type == "array" or spec.type == "map")) then
            return "config '" .. k .. "' must be " .. spec.type
        end
    end
    for k, spec in pairs(schema) do
        if spec.required and config[k] == nil then return "config '" .. k .. "' is required" end
    end
    if not outputs.encode(config) then return "config must be plain values" end
    return nil
end

-- ── Application plugins ────────────────────────────────────────────────

-- hull.build.json's bytes, or nil when the app has none. Read through an
-- exact grant: a symbolic link there is refused, not followed.
local function read_config(fs, root)
    local g, err = fs.open(root, { M.CONFIG_FILE })
    if not g then return nil, M.CONFIG_FILE .. ": " .. tostring(err) end
    local t, e = g:stat(M.CONFIG_FILE)
    if t == "absent" then g:close(); return false end
    if t ~= "file" then
        g:close()
        return nil, M.CONFIG_FILE .. " is not a regular file (" .. tostring(t or e) .. ")"
    end
    local bytes, e2 = g:read(M.CONFIG_FILE, M.LIMITS.config_bytes)
    g:close()
    if not bytes then return nil, M.CONFIG_FILE .. ": " .. tostring(e2) end
    return bytes
end

local function parse_config(bytes)
    local ok, doc = pcall(json.decode, bytes)
    if not ok or type(doc) ~= "table" or (next(doc) ~= nil and is_array(doc)) then
        return nil, M.CONFIG_FILE .. " must be a JSON object"
    end
    local extra = only_keys(doc, { plugins = true })
    if extra then return nil, M.CONFIG_FILE .. ": unknown key '" .. tostring(extra) .. "'" end
    local list = doc.plugins or {}
    if not is_array(list) then return nil, M.CONFIG_FILE .. ": plugins must be an array" end
    local seen = {}
    for i, p in ipairs(list) do
        local where = M.CONFIG_FILE .. ": plugins[" .. i .. "]"
        if type(p) ~= "table" or is_array(p) and next(p) ~= nil then
            return nil, where .. " must be an object"
        end
        extra = only_keys(p, { path = true, config = true })
        if extra then return nil, where .. ": unknown key '" .. tostring(extra) .. "'" end
        if not outputs.canonical(p.path) then
            return nil, where .. ": path must be a relative path inside the app"
        end
        if p.path:lower() == ".hull" or p.path:lower():sub(1, 6) == ".hull/" then
            return nil, where .. ": path may not be inside .hull"
        end
        if seen[p.path] then return nil, where .. ": " .. p.path .. " is named twice" end
        seen[p.path] = true
    end
    return list
end

-- The plugin directory's entry, through exact grants (no link anywhere on
-- the way). Returns the relative entry and its language.
local function find_entry(fs, root, dir)
    local cands = { { dir .. "/app.lua", "lua" }, { dir .. "/app.js", "javascript" } }
    local g, err = fs.open(root, { cands[1][1], cands[2][1] })
    if not g then return nil, dir .. ": " .. tostring(err) end
    local found = {}
    for _, c in ipairs(cands) do
        local t, e = g:stat(c[1])
        if t == "file" then found[#found + 1] = c
        elseif t ~= "absent" then
            g:close()
            return nil, c[1] .. ": " .. tostring(t ~= nil and ("a " .. t) or e)
        end
    end
    g:close()
    if #found ~= 1 then return nil, dir .. " must hold exactly one of app.lua / app.js" end
    return found[1][1], found[1][2]
end

-- The plugin's identity hash: every file of its directory, sorted by path,
-- each "<path>\0<sha256 hex>\n", hashed. A link or a special file in it is
-- refused, so the walk never leaves the directory.
local function tree_hash(fs, root, dir, sha256_hex)
    local g, err = fs.open(root, { dir .. "/" })
    if not g then return nil, dir .. ": " .. tostring(err) end
    local files, bytes, lines = 0, 0, {}
    local function walk(rel)
        local list, e = g:list(rel == "" and dir or (dir .. "/" .. rel))
        if not list then return dir .. "/" .. rel .. ": " .. tostring(e) end
        table.sort(list, function(a, b) return a.name < b.name end)
        for _, ent in ipairs(list) do
            local p = rel == "" and ent.name or (rel .. "/" .. ent.name)
            if ent.type == "dir" then
                local e2 = walk(p)
                if e2 then return e2 end
            elseif ent.type == "file" then
                files = files + 1
                if files > M.LIMITS.plugin_files then return dir .. ": too many files" end
                local b, sha = g:read(dir .. "/" .. p, M.LIMITS.file_bytes)
                if not b then return dir .. "/" .. p .. ": " .. tostring(sha) end
                bytes = bytes + #b
                if bytes > M.LIMITS.plugin_bytes then return dir .. ": too large" end
                lines[#lines + 1] = p .. "\0" .. sha .. "\n"
            else
                return dir .. "/" .. p .. ": a " .. tostring(ent.type) ..
                       " (a plugin directory holds files and directories only)"
            end
        end
        return nil
    end
    local e = walk("")
    g:close()
    if e then return nil, e end
    table.sort(lines)
    return sha256_hex(table.concat(lines))
end

local function default_extract(entry, language)
    if language == "lua" then return tool.extract_manifest_lua(entry) end
    local ok, j = pcall(tool.extract_manifest_js, entry)
    if not ok then return nil, j end
    return j
end

local function default_sha256_hex(s)
    local crypto = require("hull.crypto")
    local enc = require("hull.encoding")
    return enc.hex.encode(crypto.sha256(s))
end

-- One application plugin: located, hashed, its manifest extracted and
-- checked against the preset, its descriptor and configuration validated.
local function load_app_plugin(ctx, item)
    local rel, lang = find_entry(ctx.fs, ctx.root, item.path)
    if not rel then return nil, lang end
    local digest, e = tree_hash(ctx.fs, ctx.root, item.path, ctx.sha256_hex)
    if not digest then return nil, e end
    local entry = ctx.root .. "/" .. rel
    local mj, merr = ctx.extract(entry, lang)
    if not mj then
        return nil, item.path .. ": its manifest could not be read" ..
                    (merr and (": " .. tostring(merr)) or " (none declared)")
    end
    local ok, manifest = pcall(json.decode, mj)
    if not ok or type(manifest) ~= "table" then return nil, item.path .. ": malformed manifest" end
    local pok, why = ctx.native.preset_check(manifest)
    if not pok then return nil, item.path .. ": " .. tostring(why) end
    local raw = lang == "lua" and manifest.build_plugin or manifest.buildPlugin
    local desc, derr = M.check_descriptor(raw)
    if not desc then return nil, item.path .. ": " .. derr end
    local cerr = M.check_config(desc.config, item.config)
    if cerr then return nil, item.path .. ": " .. cerr end
    return {
        identity = "app:" .. item.path .. "#" .. digest,
        path = item.path, entry = entry, language = lang,
        descriptor = desc, config = item.config or {},
    }
end

-- The application plugin directories hull.build.json names, without loading
-- the plugins: what `hull build` leaves out of the app. A list (empty when
-- there is no file), or nil, message.
function M.plugin_paths(root, opts)
    local fs = (opts and opts.fs) or require("hull.buildctx._fs")
    local bytes, err = read_config(fs, root)
    if bytes == nil then return nil, err end
    if not bytes then return {} end
    local list, perr = parse_config(bytes)
    if not list then return nil, perr end
    local out = {}
    for i, p in ipairs(list) do out[i] = p.path end
    return out
end

-- A validated copy of a route, or nil, reason.
function M.check_route(r)
    if type(r) ~= "table" then return nil, "a route must be a table" end
    local extra = only_keys(r, { id = true, unit = true, language = true, plugin = true })
    if extra then return nil, "unknown route key '" .. tostring(extra) .. "'" end
    if type(r.id) ~= "string" or not r.id:match("^[a-z][a-z0-9%-]*$") or #r.id > 64 then
        return nil, "route id must match [a-z][a-z0-9-]* (at most 64 bytes)"
    end
    if not M.ROUTE_UNITS[r.unit] then return nil, "route " .. r.id .. ": unit must be \"module\"" end
    if not LANGUAGES[r.language] then
        return nil, "route " .. r.id .. ": language must be lua, javascript or typescript"
    end
    if type(r.plugin) ~= "string" then return nil, "route " .. r.id .. ": plugin must name a bundled plugin" end
    return { id = r.id, unit = r.unit, language = r.language, plugin = r.plugin,
             kind = r.unit .. ":" .. r.language }
end

local Registry = {}
Registry.__index = Registry

-- The plugin claiming an input kind, or nil.
function Registry:route(kind) return self.by_kind[kind] end

-- opts: fs (hull.buildctx._fs), native (hull.buildctx._plugin),
-- extract(entry, language) -> json | nil, err, sha256_hex(s), bundled.
function M.load(root, opts)
    opts = opts or {}
    if type(root) ~= "string" or root:sub(1, 1) ~= "/" then return nil, "invalid_args" end
    local ctx = {
        root = root,
        fs = opts.fs or require("hull.buildctx._fs"),
        native = opts.native or require("hull.buildctx._plugin"),
        extract = opts.extract or default_extract,
        sha256_hex = opts.sha256_hex or default_sha256_hex,
    }
    local reg = setmetatable({ plugins = {}, by_kind = {}, routes = {}, config_sha256 = nil }, Registry)
    local function add(p, claims)
        for _, k in ipairs(claims) do
            local other = reg.by_kind[k]
            if other then
                return p.identity .. " and " .. other.identity .. " both claim " .. k
            end
            reg.by_kind[k] = p
        end
        p.claims = claims
        reg.plugins[#reg.plugins + 1] = p
        return nil
    end
    -- Bundled plugins, each on its routes.
    local bundled_list, route_list = opts.bundled, opts.routes
    if not bundled_list then
        local dl, dr = M.defaults(ctx.sha256_hex)
        bundled_list, route_list = dl, route_list or dr
    end
    route_list = route_list or {}
    local bundled, routed = {}, {}
    for _, b in ipairs(bundled_list) do
        local name = "stdlib:" .. tostring(b.id)
        local desc, derr = M.check_descriptor(b.descriptor)
        if not desc then return nil, name .. ": " .. derr end
        if desc.id ~= b.id then return nil, name .. ": descriptor id is " .. desc.id end
        if math.type(b.major) ~= "integer" or b.major < 1 then
            return nil, name .. ": major must be an integer >= 1"
        end
        if bundled[b.id] then return nil, name .. " is bundled twice" end
        if not (type(b.entry) == "string" and b.entry:sub(1, 1) == "/") and type(b.files) ~= "table" then
            return nil, name .. ": no entry"
        end
        if b.language ~= "lua" and b.language ~= "javascript" then
            return nil, name .. ": language must be lua or javascript"
        end
        bundled[b.id] = { identity = name .. "@" .. b.major, descriptor = desc, config = {},
                          entry = b.entry, language = b.language, bundled = b,
                          implementation = b.implementation }
        routed[b.id] = {}
    end
    for _, r0 in ipairs(route_list) do
        local r, rerr = M.check_route(r0)
        if not r then return nil, rerr end
        local p = bundled[r.plugin]
        if not p then return nil, "route " .. r.id .. " names no bundled plugin (" .. r.plugin .. ")" end
        local d = p.descriptor
        local takes = false
        for _, l in ipairs(d.input == "source" and d.languages or {}) do
            if l == r.language then takes = true end
        end
        if not takes then
            return nil, "route " .. r.id .. ": " .. p.identity .. " takes no " .. r.language .. " source"
        end
        for _, other in ipairs(reg.routes) do
            if other.id == r.id then return nil, "route " .. r.id .. " is defined twice" end
        end
        r.plugin = p
        reg.routes[#reg.routes + 1] = r
        local list = routed[p.descriptor.id]
        list[#list + 1] = r.kind
    end
    for _, b in ipairs(bundled_list) do
        local p = bundled[b.id]
        if #routed[b.id] == 0 then return nil, p.identity .. " is on no route" end
        local e = add(p, routed[b.id])
        if e then return nil, e end
    end
    table.sort(reg.routes, function(a, b) return a.id < b.id end)
    local bytes, err = read_config(ctx.fs, root)
    if bytes == nil then return nil, err end
    if bytes then
        reg.config_sha256 = ctx.sha256_hex(bytes)
        local list, perr = parse_config(bytes)
        if not list then return nil, perr end
        for _, item in ipairs(list) do
            local p, e = load_app_plugin(ctx, item)
            if not p then return nil, e end
            e = add(p, M.claims(p.descriptor))
            if e then return nil, e end
        end
    end
    return reg
end

return M
