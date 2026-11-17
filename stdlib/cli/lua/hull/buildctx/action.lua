-- hull.buildctx.action - the BuildAction core and its declared workspace inputs
-- (docs/buildcontext_design.md §2, §4; checkpoint 1 of §13, recorded in §13.1).
--
-- Host-owned tool code: it runs in the tool VM, never in a plugin's runtime, and
-- holds the one native piece, hull.buildctx._fs (the grants compiled with the
-- shipped fs policy, and read / stat / list through it).
--
--   local action = require("hull.buildctx.action")
--   local a, err = action.new({ root = "/abs/app", grants = { "src/" } })
--   a:start()
--   local bytes, err = a:read("src/a.lua")       -- recorded, repeatable
--   local st, err    = a:stat("bin/x", { exec = true })
--   local list, err  = a:list("src")             -- { {name=, type=}, ... }
--   a:prepare()                                  -- freezes; builds the manifest
--   a:commit()
--   local manifest   = a:deps()
--   local fresh, err = action.check(root, manifest)
--
-- Lifecycle, monotonic: created -> running -> preparing -> committed; created /
-- running / preparing -> aborted. Errors are nil, token. A method on a committed
-- or aborted action answers "stale_handle"; on the wrong live state "bad_state".
--
-- Each input op has one of three outcomes:
--   denied   - "permission", "invalid_path", "path_too_deep", "reserved_path":
--              depends only on the grants; returned, not recorded;
--   observed - content, a type, or the workspace's answer ("not_found",
--              "not_a_regular_file", ...): recorded, and the first observation of
--              a (op, path, fields) is canonical - a repeat returns it whatever the
--              tree holds by then;
--   failed   - "limit", "io_error", anything else: fails the op AND the action,
--              so prepare answers "input_failed" and no result can lack an input
--              its plugin read.

local json = require("hull.json")

local M = {}

M.FORMAT = "hull-build-deps"
M.VERSION = 1
M.DEFAULT_LIMITS = {
    max_read_bytes   = 16 * 1024 * 1024,   -- one read
    max_total_bytes  = 256 * 1024 * 1024,  -- every captured byte of the action
    max_observations = 65536,              -- distinct (op, path, fields)
}

local DENIED = {
    permission = true, invalid_path = true, path_too_deep = true, reserved_path = true,
    invalid_args = true,
}
local OBSERVED = {
    not_found = true, not_a_regular_file = true, is_a_directory = true,
    not_a_directory = true, symlink_denied = true, symlink_loop = true,
}
local OP_ORDER = { list = 1, read = 2, stat = 3 }

-- One file, one spelling: relative, '/'-separated, no empty, "." or ".." part
-- ("." alone names the root, for stat and list), and valid UTF-8 with no control
-- byte, so every path round-trips through the JSON manifest unchanged.
local function path_ok(path, root_ok)
    if type(path) ~= "string" or #path == 0 or #path >= 4096 then return false end
    if path == "." then return root_ok end
    if not utf8.len(path) or path:find("[%c\127]") then return false end
    if path:sub(1, 1) == "/" or path:sub(-1) == "/" then return false end
    for part in (path .. "/"):gmatch("([^/]*)/") do
        if part == "" or part == "." or part == ".." then return false end
    end
    return true
end

local function fields_key(fields)
    local f = {}
    if fields and fields.exec then f[#f + 1] = "exec" end
    if fields and fields.size then f[#f + 1] = "size" end
    return f, table.concat(f, ",")
end

-- Perform one observation (not recorded). Returns the observation table, or
-- nil, token, outcome ("denied" / "failed").
local function observe(g, op, path, fkey, limits)
    if op == "read" then
        local bytes, sha = g:read(path, limits.max_read_bytes)
        if bytes then return { bytes = bytes, sha256 = sha, size = #bytes } end
        if DENIED[sha] then return nil, sha, "denied" end
        if OBSERVED[sha] then return { result = sha } end
        return nil, sha, "failed"
    elseif op == "stat" then
        local t, size, exec = g:stat(path)
        if not t then
            if DENIED[size] then return nil, size, "denied" end
            if OBSERVED[size] then return { result = size } end
            return nil, size, "failed"
        end
        local o = { type = t }
        if t ~= "absent" then
            if fkey:find("size", 1, true) then o.size = size end
            if fkey:find("exec", 1, true) then o.exec = exec end
        end
        return o
    else
        local list, sha = g:list(path)
        if list then return { entries = list, sha256 = sha } end
        if DENIED[sha] then return nil, sha, "denied" end
        if OBSERVED[sha] then return { result = sha } end
        return nil, sha, "failed"
    end
end

-- Two observations of the same (op, path, fields) saw the same thing: a read or
-- a list by its hash, a stat by its type and the fields it recorded.
local function same(op, a, b)
    if a.result or b.result then return a.result == b.result end
    if op ~= "stat" then return a.sha256 == b.sha256 end
    return a.type == b.type and a.size == b.size and a.exec == b.exec
end

local Action = {}
Action.__index = Action

-- spec: { root = absolute path, grants = { ... }, limits = { ... } }.
-- opts.fs replaces hull.buildctx._fs (tests).
function M.new(spec, opts)
    if type(spec) ~= "table" or type(spec.root) ~= "string" then return nil, "invalid_args" end
    local fs = (opts and opts.fs) or require("hull.buildctx._fs")
    local grants = {}
    for i, gr in ipairs(spec.grants or {}) do
        if type(gr) ~= "string" then return nil, "invalid_args" end
        grants[i] = gr
    end
    local limits = {}
    for k, v in pairs(M.DEFAULT_LIMITS) do
        local s = spec.limits and spec.limits[k]
        limits[k] = (math.type(s) == "integer" and s > 0) and s or v
    end
    local g, err = fs.open(spec.root, grants)
    if not g then return nil, err end
    return setmetatable({
        state = "created", grants = grants, limits = limits, g = g,
        obs = {}, n_obs = 0, total = 0, failed = false, cancelled = false,
    }, Action)
end

local function terminal(self) return self.state == "committed" or self.state == "aborted" end

local function close_inputs(self)
    if self.g then self.g:close(); self.g = nil end
end

function Action:abort()
    if terminal(self) then return true end
    self.state = "aborted"
    self.manifest = nil
    close_inputs(self)
    return true
end

-- Ask the action to stop: its next host call (an input op, prepare, commit)
-- aborts it and answers "cancelled".
function Action:cancel() self.cancelled = true end

local function take_cancel(self)
    if self.cancelled and not terminal(self) then
        self:abort()
        return true
    end
    return false
end

-- The state check every entry runs: nil, token when the call may not proceed.
local function gate(self, want)
    if take_cancel(self) then return "cancelled" end
    if want == "running" and terminal(self) then return "stale_handle" end
    if self.state ~= want then return "bad_state" end
    return nil
end

function Action:start()
    local e = gate(self, "created")
    if e then return nil, e end
    self.state = "running"
    return true
end

-- The recorded observation of (op, path, fields): made now on first use.
function Action:_record(op, path, fields)
    local e = gate(self, "running")
    if e then return nil, e end
    if not path_ok(path, op ~= "read") then return nil, "invalid_path" end
    local flist, fkey = fields_key(fields)
    local key = op .. "\0" .. fkey .. "\0" .. path
    local o = self.obs[key]
    if not o then
        if self.n_obs >= self.limits.max_observations then
            self.failed = true
            return nil, "limit"
        end
        local tok, outcome
        o, tok, outcome = observe(self.g, op, path, fkey, self.limits)
        if not o then
            if outcome == "failed" then self.failed = true end
            return nil, tok
        end
        local bytes = (o.bytes and #o.bytes or 0)
        for _, ent in ipairs(o.entries or {}) do bytes = bytes + #ent.name + 1 end
        if bytes > self.limits.max_total_bytes - self.total then
            self.failed = true
            return nil, "limit"
        end
        self.total = self.total + bytes
        o.op, o.path, o.fields = op, path, flist
        self.obs[key] = o
        self.n_obs = self.n_obs + 1
    end
    if o.result then return nil, o.result end
    return o
end

function Action:read(path)
    local o, e = self:_record("read", path)
    if not o then return nil, e end
    return o.bytes
end

-- fields: { size = true, exec = true }; the type is always recorded. Returns
-- { type = "file"|"dir"|"symlink"|"other"|"absent", size?, exec? }.
function Action:stat(path, fields)
    local o, e = self:_record("stat", path, fields)
    if not o then return nil, e end
    return { type = o.type, size = o.size, exec = o.exec }
end

function Action:list(path)
    local o, e = self:_record("list", path)
    if not o then return nil, e end
    return o.entries
end

-- The canonical manifest: grants in declared order, observations sorted by
-- path (bytes), then op (list, read, stat), then fields. No mtime anywhere.
local function encode(grants, obs)
    local sorted = {}
    for _, o in pairs(obs) do sorted[#sorted + 1] = o end
    table.sort(sorted, function(a, b)
        if a.path ~= b.path then return a.path < b.path end
        if a.op ~= b.op then return OP_ORDER[a.op] < OP_ORDER[b.op] end
        return table.concat(a.fields, ",") < table.concat(b.fields, ",")
    end)
    local s = json.encode
    local out = { '{"format":', s(M.FORMAT), ',"version":', tostring(M.VERSION), ',"grants":[' }
    for i, gr in ipairs(grants) do out[#out + 1] = (i > 1 and "," or "") .. s(gr) end
    out[#out + 1] = '],"observations":['
    for i, o in ipairs(sorted) do
        local p = { '{"op":', s(o.op), ',"path":', s(o.path) }
        if o.op == "stat" then
            local f = {}
            for j, name in ipairs(o.fields) do f[j] = s(name) end
            p[#p + 1] = ',"fields":[' .. table.concat(f, ",") .. "]"
        end
        if o.result then
            p[#p + 1] = ',"result":' .. s(o.result)
        elseif o.op == "read" then
            p[#p + 1] = ',"sha256":' .. s(o.sha256) .. ',"size":' .. tostring(o.size)
        elseif o.op == "list" then
            p[#p + 1] = ',"sha256":' .. s(o.sha256) .. ',"entries":' .. tostring(#o.entries)
        else
            p[#p + 1] = ',"type":' .. s(o.type)
            if o.exec ~= nil then p[#p + 1] = ',"exec":' .. tostring(o.exec) end
            if o.size ~= nil then p[#p + 1] = ',"size":' .. tostring(o.size) end
        end
        out[#out + 1] = (i > 1 and "," or "") .. table.concat(p) .. "}"
    end
    out[#out + 1] = "]}"
    return table.concat(out)
end

function Action:prepare()
    local e = gate(self, "running")
    if e then return nil, e == "stale_handle" and "bad_state" or e end
    if self.failed then
        self:abort()
        return nil, "input_failed"
    end
    self.manifest = encode(self.grants, self.obs)
    close_inputs(self)
    self.state = "preparing"
    return true
end

-- Checkpoint 1 has no result store: commit only moves the action to committed
-- (checkpoint 2 adds the staged result and its atomic install).
function Action:commit()
    local e = gate(self, "preparing")
    if e then return nil, e end
    self.state = "committed"
    return true
end

-- The dependency manifest, from preparing on.
function Action:deps()
    if self.state ~= "preparing" and self.state ~= "committed" then return nil, "bad_state" end
    return self.manifest
end

function Action:get_state() return self.state end

-- Rebuild a recorded observation from the manifest, or nil when malformed.
local function parse_obs(j)
    if type(j) ~= "table" or not OP_ORDER[j.op] then return nil end
    if not path_ok(j.path, j.op ~= "read") then return nil end
    local fields = {}
    if j.op == "stat" then
        if type(j.fields) ~= "table" then return nil end
        for _, f in ipairs(j.fields) do
            if f ~= "size" and f ~= "exec" then return nil end
            fields[f] = true
        end
    end
    local o = { op = j.op, path = j.path, fields = fields }
    if j.result ~= nil then
        if not OBSERVED[j.result] then return nil end
        o.want = { result = j.result }
        return o
    end
    if j.op == "stat" then
        if type(j.type) ~= "string" then return nil end
        o.want = { type = j.type, size = j.size, exec = j.exec }
    else
        if type(j.sha256) ~= "string" or not j.sha256:match("^%x+$") or #j.sha256 ~= 64 then return nil end
        o.want = { sha256 = j.sha256 }
    end
    return o
end

-- Re-evaluate a manifest against the workspace at @root: compile its grants,
-- replay every observation, compare. Returns true (fresh) / false (stale), or
-- nil, token for a manifest that is malformed or of another version
-- ("bad_manifest"), or grants that no longer compile.
function M.check(root, manifest, opts)
    local fs = (opts and opts.fs) or require("hull.buildctx._fs")
    local ok, doc = pcall(json.decode, manifest)
    if not ok or type(doc) ~= "table" or doc.format ~= M.FORMAT or doc.version ~= M.VERSION or
       type(doc.grants) ~= "table" or type(doc.observations) ~= "table" then
        return nil, "bad_manifest"
    end
    for _, gr in ipairs(doc.grants) do
        if type(gr) ~= "string" then return nil, "bad_manifest" end
    end
    local want = {}
    for i, j in ipairs(doc.observations) do
        want[i] = parse_obs(j)
        if not want[i] then return nil, "bad_manifest" end
    end
    local g, err = fs.open(root, doc.grants)
    if not g then return nil, err end
    local fresh = true
    for _, w in ipairs(want) do
        local _, fkey = fields_key(w.fields)
        local cur = observe(g, w.op, w.path, fkey, M.DEFAULT_LIMITS)
        if not cur or not same(w.op, w.want, cur) then fresh = false; break end
    end
    g:close()
    return fresh
end

M._path_ok = path_ok   -- tests

return M
