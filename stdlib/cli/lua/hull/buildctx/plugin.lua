-- hull.buildctx.plugin - the build host's side of a build plugin
-- (docs/buildcontext_design.md §9.1): the process supervisor, the action
-- loop and the host operations a plugin's ctx calls name.
--
-- Host-owned tool code. The process and its socket are
-- hull.buildctx._plugin; the action - inputs, outputs, diagnostics, prepare
-- and commit - is hull.buildctx.action. This module only drives one through
-- the other: every operation, its argument checks and its error tokens are
-- defined here, once, for plugins in either language.
--
--   local plugin = require("hull.buildctx.plugin")
--   local r = plugin.new({ exe = hull, entry = "/abs/plugins/x/app.lua" })
--   local a = action.new({ root, grants, key, store })
--   local ok, tok, detail = r:run(a, { id = 1, config = {...}, meta = {...},
--                                      source = { units = {...} } })
--   ...                                       -- more actions, one process
--   r:close()                                 -- STOP; the process exits
--
-- One process serves a runner's actions one at a time. A process that
-- crashes, misses the action's deadline or breaks the protocol aborts its
-- action and is killed; the next action gets a fresh one. A transform that
-- fails (DONE with an error) aborts its action only: the process is reused.
--
-- run() starts the action, serves the plugin's calls until it reports DONE,
-- then prepares and commits it. It answers true, or nil, token, detail:
--   plugin_failed    the transform raised (detail: its message)
--   plugin_crashed   the process exited or closed the channel
--   plugin_timeout   no DONE within limits.action_ms
--   plugin_protocol  a frame or a value outside the protocol
--   spawn_failed     the process could not be started
--   host_error       an operation raised on this side (detail: the error)
--   cancelled        action:cancel() was called while it ran
--   anything prepare / commit answers (diagnostic_error, open_writer, ...)

local M = {}

M.DEFAULT_LIMITS = {
    action_ms = 120000,      -- one action, from ACTION to DONE
    stop_ms   = 5000,        -- how long a stopped process may take to exit
}

-- The plugin runs with no database on disk: its directory is its app
-- directory, and an implicit data.db there would change its identity.
M.PLUGIN_ARGS = { "-d", ":memory:" }

local Runner = {}
Runner.__index = Runner

function M.new(spec, opts)
    if type(spec) ~= "table" or type(spec.exe) ~= "string" or type(spec.entry) ~= "string" or
       spec.entry:sub(1, 1) ~= "/" then
        return nil, "invalid_args"
    end
    local limits = {}
    for k, v in pairs(M.DEFAULT_LIMITS) do
        local s = spec.limits and spec.limits[k]
        limits[k] = (math.type(s) == "integer" and s > 0) and s or v
    end
    return setmetatable({
        native = (opts and opts.native) or require("hull.buildctx._plugin"),
        exe = spec.exe, entry = spec.entry, limits = limits,
        proc = nil, next_handle = 0, spawned = 0,
    }, Runner)
end

local function kill(self)
    if self.proc then self.proc:kill(); self.proc = nil end
end

local function spawn(self)
    local p, err, why = self.native.spawn(self.exe, self.entry, M.PLUGIN_ARGS)
    if not p then return nil, why and (err .. ": " .. why) or err end
    self.proc = p
    self.spawned = self.spawned + 1
    return p
end

-- ── The host operations ────────────────────────────────────────────────
-- Each takes the action's serving state and the call's args, and answers
-- the reply's ok value (nil for none) or nil, token.

local function str(v) return type(v) == "string" end

local OPS = {}

OPS["inputs.read"] = function(s, a)
    if not str(a.path) then return nil, "invalid_args" end
    local b, e = s.action:read(a.path)
    if not b then return nil, e end
    return s.native.bytes(b)
end

OPS["inputs.stat"] = function(s, a)
    if not str(a.path) or (a.opts ~= nil and type(a.opts) ~= "table") then
        return nil, "invalid_args"
    end
    local o = a.opts or {}
    local st, e = s.action:stat(a.path, { size = o.size == true, exec = o.exec == true })
    if not st then return nil, e end
    return st
end

OPS["inputs.list"] = function(s, a)
    if not str(a.path) then return nil, "invalid_args" end
    local list, e = s.action:list(a.path)
    if not list then return nil, e end
    return #list > 0 and list or s.native.empty_array
end

-- A writer the plugin names by the id this host gave it, in this action.
local function handle(s, a, kind)
    local h = math.type(a.handle) == "integer" and s.handles[a.handle]
    if not h or h.kind ~= kind then return nil end
    return h
end

local function create(s, a, method)
    if not str(a.name) then return nil, "invalid_args" end
    local h, e = s.action[method](s.action, a.name)
    if not h then return nil, e end
    s.runner.next_handle = s.runner.next_handle + 1
    s.handles[s.runner.next_handle] = h
    return s.runner.next_handle
end

OPS["outputs.create_blob"] = function(s, a) return create(s, a, "create_blob") end
OPS["outputs.create_tree"] = function(s, a) return create(s, a, "create_tree") end

local function done(ok, e)
    if not ok then return nil, e end
    return nil
end

OPS["blob.write"] = function(s, a)
    local h = handle(s, a, "blob")
    if not h then return nil, "invalid_handle" end
    if not str(a.bytes) then return nil, "invalid_args" end
    return done(h:write(a.bytes))
end

OPS["blob.close"] = function(s, a)
    local h = handle(s, a, "blob")
    if not h then return nil, "invalid_handle" end
    return done(h:close())
end

OPS["tree.write"] = function(s, a)
    local h = handle(s, a, "tree")
    if not h then return nil, "invalid_handle" end
    if not str(a.path) or not str(a.bytes) or (a.mode ~= nil and not str(a.mode)) then
        return nil, "invalid_args"
    end
    return done(h:write(a.path, a.bytes, a.mode))
end

OPS["tree.close"] = function(s, a)
    local h = handle(s, a, "tree")
    if not h then return nil, "invalid_handle" end
    return done(h:close())
end

local function declare(s, a, method)
    local h = math.type(a.handle) == "integer" and s.handles[a.handle]
    if not h then return nil, "invalid_handle" end
    if type(a.descriptor) ~= "table" then return nil, "invalid_descriptor" end
    return done(s.action[method](s.action, a.descriptor, h))
end

OPS["outputs.declare_generated_output"] = function(s, a)
    return declare(s, a, "declare_generated_output")
end
OPS["outputs.declare_artifact"] = function(s, a) return declare(s, a, "declare_artifact") end

OPS["diagnostics.emit"] = function(s, a)
    if type(a.diagnostic) ~= "table" then return nil, "invalid_diagnostic" end
    return done(s.action:emit(a.diagnostic))
end

-- The action's source selection, as the caller planned it.
OPS["source.units"] = function(s)
    local units = s.source.units
    if type(units) ~= "table" or #units == 0 then return s.native.empty_array end
    return units
end

-- A frontend operation on one unit of the selection (a.unit: its id);
-- answered by the caller's frontend session when it offers one, as
-- { bytes?, refusals = { diagnostic... } } - bytes absent with any refusal.
OPS["source.erased_source"] = function(s, a)
    if not str(a.unit) then return nil, "invalid_args" end
    local fn = s.source.erased_source
    if type(fn) ~= "function" then return nil, "unsupported_op" end
    local r, e = fn(a.unit)
    if type(r) ~= "table" then return nil, e or "io_error" end
    local refusals = type(r.refusals) == "table" and r.refusals or {}
    if #refusals > 0 then return { refusals = refusals } end
    if type(r.bytes) ~= "string" then return nil, "io_error" end
    return { bytes = s.native.bytes(r.bytes), refusals = s.native.empty_array }
end

-- The reply to one CALL value: { ok = v } or { error = token }. An action
-- that ended (committed, aborted, cancelled) answers every op as the action
-- itself does.
local function answer(s, call)
    local op = type(call) == "table" and call.op
    local fn = str(op) and OPS[op]
    if not fn then return { error = "unknown_op" } end
    local args = call.args
    if args == nil then args = {} end
    if type(args) ~= "table" then return { error = "invalid_args" } end
    local v, e = fn(s, args)
    if v == nil and e ~= nil then return { error = e } end
    if v == nil then return {} end
    return { ok = v }
end

-- ── The action loop ────────────────────────────────────────────────────

-- Serve one action until DONE. Answers true, or nil, token, detail; on a
-- token the process is gone (killed) unless it is plugin_failed.
local function serve(self, s)
    local deadline = s.native_now() + self.limits.action_ms
    while true do
        local left = deadline - s.native_now()
        if left <= 0 then
            kill(self)
            return nil, "plugin_timeout"
        end
        local t, id, v = self.proc:recv(left)
        if not t then
            kill(self)
            if id == "timeout" then return nil, "plugin_timeout" end
            if id == "eof" then return nil, "plugin_crashed" end
            return nil, "plugin_protocol", id
        end
        if t == self.native.DONE then
            if type(v) ~= "table" or (v.error ~= nil and not str(v.error)) then
                kill(self)
                return nil, "plugin_protocol", "done"
            end
            if v.error ~= nil then return nil, "plugin_failed", v.error end
            return true
        end
        if t ~= self.native.CALL or type(v) ~= "table" then
            kill(self)
            return nil, "plugin_protocol", "frame"
        end
        local fine, reply = pcall(answer, s, v)
        if not fine then
            kill(self)
            return nil, "host_error", reply
        end
        local ok, e = self.proc:send(self.native.REPLY, id, reply, left)
        if not ok then
            kill(self)
            return nil, e == "eof" and "plugin_crashed" or "plugin_protocol", e
        end
    end
end

-- req: { id = integer, config = table?, meta = table?,
--        source = { units = {...},
--                   erased_source = fn(unit_id) -> { bytes?, refusals } | nil, token }? }
function Runner:run(action, req)
    if type(req) ~= "table" or math.type(req.id) ~= "integer" then return nil, "invalid_args" end
    local ok, e = action:start()
    if not ok then return nil, e end
    local msg = { id = req.id, config = req.config or {}, meta = req.meta or {} }
    -- A process that went while idle is replaced before the action starts.
    local sent
    for _ = 1, 2 do
        if not self.proc then
            local p, err = spawn(self)
            if not p then
                action:abort()
                return nil, "spawn_failed", err
            end
        end
        sent, e = self.proc:send(self.native.ACTION, 0, msg)
        if sent then break end
        kill(self)
        if e ~= "eof" then
            action:abort()
            return nil, "plugin_protocol", e
        end
    end
    if not sent then
        action:abort()
        return nil, "plugin_crashed"
    end
    local s = {
        runner = self, action = action, native = self.native, handles = {},
        source = type(req.source) == "table" and req.source or {},
        native_now = self.native.now,
    }
    local served, tok, detail = serve(self, s)
    if action:get_state() == "aborted" then
        return nil, "cancelled"
    end
    if not served then
        action:abort()
        return nil, tok, detail
    end
    ok, e = action:prepare()
    if not ok then return nil, e end
    ok, e = action:commit()
    if not ok then return nil, e end
    return true
end

-- How many processes this runner has started (tests: replacement).
function Runner:spawn_count() return self.spawned end

-- No more actions: STOP, and wait for the process to exit. Answers its exit
-- status (0 for a clean stop), "killed", or nil when none was running.
function Runner:close()
    local p = self.proc
    if not p then return nil end
    self.proc = nil
    p:send(self.native.STOP, 0, nil, self.limits.stop_ms)
    return p:close(self.limits.stop_ms)
end

return M
