-- hull.buildctx.outputs - a BuildAction's private result construction and its
-- diagnostics (docs/buildcontext_design.md §6, §8, §13.2). Host-owned tool
-- code, driven by hull.buildctx.action.
--
-- Staging is in memory, bounded by the limits: nothing reaches the store before
-- the action prepares, and nothing is visible before it commits. A plugin has no
-- publication primitive - it builds payloads and declares them.
--
--   local w = o:create_blob("app")              -- a payload handle
--   w:write(bytes) ... w:close()
--   local t = o:create_tree("assets")
--   t:write("css/site.css", bytes, "regular")   -- or "executable"
--   t:close()
--   o:declare_generated_output({ name = "./app.ts" }, w)
--   o:declare_artifact({ name = "wasm/m", kind = "wasm-module" }, t)
--   o:emit({ severity = "warning", code = "x.y", message = "..." })
--
-- Every method answers nil, token on refusal. Generic constraints only (§6):
-- the descriptor schemas land with BuildArtifact.

local json = require("hull.json")

local M = {}

M.DEFAULT_LIMITS = {
    max_output_bytes = 32 * 1024 * 1024,   -- every staged byte of the action
    max_output_files = 4096,               -- blobs plus tree entries
    max_tree_depth   = 32,                 -- path components in a tree
}

local SEVERITIES = { error = true, warning = true, info = true }
local MODES = { regular = true, executable = true }

-- One spelling per name: relative, '/'-separated, no empty / "." / ".." part,
-- valid UTF-8 with no control byte (the action's input rule).
local function canonical(path)
    if type(path) ~= "string" or #path == 0 or #path >= 4096 then return false end
    if not utf8.len(path) or path:find("[%c\127]") then return false end
    if path:sub(1, 1) == "/" or path:sub(-1) == "/" then return false end
    for part in (path .. "/"):gmatch("([^/]*)/") do
        if part == "" or part == "." or part == ".." then return false end
    end
    return true
end
M.canonical = canonical

local function depth(path)
    local n = 1
    for _ in path:gmatch("/") do n = n + 1 end
    return n
end

-- Canonical JSON of a plain value: maps with sorted string keys, arrays,
-- strings, integers, booleans. Anything else (a float, a function, a table that
-- is neither) is refused, so the result's bytes are a function of its content.
local function canon(v, out, d)
    if d > 16 then return false end
    local t = type(v)
    if t == "string" then
        if not utf8.len(v) then return false end
        out[#out + 1] = json.encode(v)
    elseif t == "boolean" then
        out[#out + 1] = tostring(v)
    elseif math.type(v) == "integer" then
        out[#out + 1] = tostring(v)
    elseif t == "table" then
        if getmetatable(v) ~= nil then return false end
        local n, keys = 0, {}
        for k in pairs(v) do
            n = n + 1
            keys[n] = k
        end
        local is_array = n > 0
        for i = 1, n do
            if v[i] == nil then is_array = false; break end
        end
        if is_array then
            out[#out + 1] = "["
            for i = 1, n do
                if i > 1 then out[#out + 1] = "," end
                if not canon(v[i], out, d + 1) then return false end
            end
            out[#out + 1] = "]"
        else
            for _, k in ipairs(keys) do
                if type(k) ~= "string" or not utf8.len(k) then return false end
            end
            table.sort(keys)
            out[#out + 1] = "{"
            for i, k in ipairs(keys) do
                if i > 1 then out[#out + 1] = "," end
                out[#out + 1] = json.encode(k) .. ":"
                if not canon(v[k], out, d + 1) then return false end
            end
            out[#out + 1] = "}"
        end
    else
        return false
    end
    return true
end

function M.encode(v)
    local out = {}
    if not canon(v, out, 0) then return nil end
    return table.concat(out)
end

local Outputs = {}
Outputs.__index = Outputs

function M.new(limits)
    local lim = {}
    for k, v in pairs(M.DEFAULT_LIMITS) do
        local s = limits and limits[k]
        lim[k] = (math.type(s) == "integer" and s > 0) and s or v
    end
    return setmetatable({
        limits = lim, live = true, bytes = 0, files = 0,
        payloads = {},       -- name -> handle
        order = {},          -- handles in creation order
        declared = {},       -- { kind, descriptor, handle }
        names = {},          -- "kind\0name" -> true
        diagnostics = {},
        has_error = false,
    }, Outputs)
end

-- Count @n bytes and @files more files against the limits.
local function charge(self, n, files)
    if n > self.limits.max_output_bytes - self.bytes then return "limit" end
    if files > self.limits.max_output_files - self.files then return "limit" end
    self.bytes = self.bytes + n
    self.files = self.files + files
    return nil
end

local function new_handle(self, kind, name)
    if not self.live then return nil, "stale_handle" end
    if not canonical(name) then return nil, "invalid_name" end
    if self.payloads[name] then return nil, "duplicate_payload" end
    local h = { kind = kind, name = name, open = true, owner = self }
    if kind == "blob" then h.chunks = {}
    else h.entries = {}; h.dirs = {} end
    self.payloads[name] = h
    self.order[#self.order + 1] = h
    return h
end

local Blob, Tree = {}, {}
Blob.__index, Tree.__index = Blob, Tree

function Outputs:create_blob(name)
    local h, e = new_handle(self, "blob", name)
    if not h then return nil, e end
    local e2 = charge(self, 0, 1)
    if e2 then return nil, e2 end
    return setmetatable(h, Blob)
end

function Outputs:create_tree(name)
    local h, e = new_handle(self, "tree", name)
    if not h then return nil, e end
    return setmetatable(h, Tree)
end

local function writable(h)
    if not h.owner.live then return "stale_handle" end
    if not h.open then return "closed" end
    return nil
end

function Blob:write(bytes)
    local e = writable(self)
    if e then return nil, e end
    if type(bytes) ~= "string" then return nil, "invalid_args" end
    e = charge(self.owner, #bytes, 0)
    if e then return nil, e end
    self.chunks[#self.chunks + 1] = bytes
    return true
end

function Blob:close()
    local e = writable(self)
    if e then return nil, e end
    self.bytes = table.concat(self.chunks)
    self.chunks = nil
    self.open = false
    return true
end

-- A tree holds files only; a path may not be both a file and a directory.
function Tree:write(path, bytes, mode)
    local e = writable(self)
    if e then return nil, e end
    if not canonical(path) then return nil, "invalid_path" end
    if type(bytes) ~= "string" then return nil, "invalid_args" end
    mode = mode or "regular"
    if not MODES[mode] then return nil, "invalid_mode" end
    if depth(path) > self.owner.limits.max_tree_depth then return nil, "limit" end
    if self.entries[path] then return nil, "duplicate_path" end
    if self.dirs[path] then return nil, "path_conflict" end
    local prefix = ""
    for part in path:gmatch("([^/]+)/") do
        prefix = prefix == "" and part or (prefix .. "/" .. part)
        if self.entries[prefix] then return nil, "path_conflict" end
    end
    e = charge(self.owner, #bytes, 1)
    if e then return nil, e end
    prefix = ""
    for part in path:gmatch("([^/]+)/") do
        prefix = prefix == "" and part or (prefix .. "/" .. part)
        self.dirs[prefix] = true
    end
    self.entries[path] = { bytes = bytes, mode = mode }
    return true
end

function Tree:close()
    local e = writable(self)
    if e then return nil, e end
    self.open = false
    return true
end

-- A handle this action created, closed, and not declared yet.
local function declarable(self, handle)
    if not self.live then return "stale_handle" end
    if type(handle) ~= "table" or handle.owner ~= self then return "invalid_handle" end
    if handle.open then return "open_writer" end
    if handle.declared then return "already_declared" end
    return nil
end

local function declare(self, kind, desc, handle, name_ok)
    local e = declarable(self, handle)
    if e then return nil, e end
    if type(desc) ~= "table" or not name_ok(desc.name) then return nil, "invalid_descriptor" end
    local enc = M.encode(desc)
    if not enc then return nil, "invalid_descriptor" end
    local key = kind .. "\0" .. desc.name
    if self.names[key] then return nil, "duplicate_output" end
    self.names[key] = true
    handle.declared = true
    self.declared[#self.declared + 1] = { kind = kind, name = desc.name, descriptor = enc, handle = handle }
    return true
end

-- A generated output is named for its source: "./app.ts" or "app.ts".
local function generated_name_ok(name)
    if type(name) ~= "string" then return false end
    return canonical(name:sub(1, 2) == "./" and name:sub(3) or name)
end

function Outputs:declare_generated_output(desc, handle)
    return declare(self, "generated", desc, handle, generated_name_ok)
end

function Outputs:declare_artifact(desc, handle)
    return declare(self, "artifact", desc, handle, canonical)
end

-- desc: { severity, code, message, path?, range? = { start, stop } }.
function Outputs:emit(d)
    if not self.live then return nil, "stale_handle" end
    if type(d) ~= "table" or not SEVERITIES[d.severity] or type(d.code) ~= "string" or
       type(d.message) ~= "string" then
        return nil, "invalid_diagnostic"
    end
    local keep = { severity = d.severity, code = d.code, message = d.message }
    if d.path ~= nil then
        if not canonical(d.path) then return nil, "invalid_diagnostic" end
        keep.path = d.path
    end
    if d.range ~= nil then
        local r = d.range
        if type(r) ~= "table" or math.type(r.start) ~= "integer" or math.type(r.stop) ~= "integer" or
           r.start < 0 or r.stop < r.start then
            return nil, "invalid_diagnostic"
        end
        keep.range = { start = r.start, stop = r.stop }
    end
    local enc = M.encode(keep)
    if not enc then return nil, "invalid_diagnostic" end
    self.diagnostics[#self.diagnostics + 1] = enc
    if d.severity == "error" then self.has_error = true end
    return true
end

-- PREPARING's checks (§2.1): every writer closed, every payload declared, no
-- error diagnostic. Freezes the outputs. nil when valid, else a token.
function Outputs:freeze()
    self.live = false
    for _, h in ipairs(self.order) do
        if h.open then return "open_writer" end
        if not h.declared then return "undeclared_payload" end
    end
    if self.has_error then return "diagnostic_error" end
    return nil
end

-- Every payload's bytes, each once: blob bytes and tree entry bytes.
function Outputs:each_payload(fn)
    for _, d in ipairs(self.declared) do
        local h = d.handle
        if h.kind == "blob" then
            fn(h.bytes)
        else
            for _, ent in pairs(h.entries) do fn(ent.bytes) end
        end
    end
end

-- The outputs and diagnostics as canonical JSON fragments, given each payload's
-- hash (sha_of(bytes) -> hex). Outputs sorted by kind, then name; tree entries
-- by path; diagnostics in the order they were emitted.
function Outputs:encode_result(sha_of)
    local decl = {}
    for i, d in ipairs(self.declared) do decl[i] = d end
    table.sort(decl, function(a, b)
        if a.kind ~= b.kind then return a.kind < b.kind end
        return a.name < b.name
    end)
    local outs = {}
    for i, d in ipairs(decl) do
        local h = d.handle
        local payload
        if h.kind == "blob" then
            payload = '{"sha256":' .. json.encode(sha_of(h.bytes)) .. ',"size":' .. #h.bytes ..
                      ',"type":"blob"}'
        else
            local paths = {}
            for p in pairs(h.entries) do paths[#paths + 1] = p end
            table.sort(paths)
            local ents = {}
            for j, p in ipairs(paths) do
                local ent = h.entries[p]
                ents[j] = '{"mode":' .. json.encode(ent.mode) .. ',"path":' .. json.encode(p) ..
                          ',"sha256":' .. json.encode(sha_of(ent.bytes)) .. ',"size":' .. #ent.bytes .. "}"
            end
            payload = '{"entries":[' .. table.concat(ents, ",") .. '],"type":"tree"}'
        end
        outs[i] = '{"declared":' .. json.encode(d.kind) .. ',"descriptor":' .. d.descriptor ..
                  ',"payload":' .. payload .. "}"
    end
    return "[" .. table.concat(outs, ",") .. "]", "[" .. table.concat(self.diagnostics, ",") .. "]"
end

return M
