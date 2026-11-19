-- hull._build_plugin - app.transform for a Lua build plugin
-- (docs/buildcontext_design.md §9.1). Internal: installed by the runtime
-- when the manifest declares hull/build-plugin@1 (runtime/lua/build_plugin.c),
-- which calls the function this module returns with the app and the channel.
--
-- Every ctx call is one request to the build host, which defines the
-- operation, checks its arguments and owns its authority; this side only
-- names it. A refused call returns nil, token (the host's error token).
--
--   app.transform(function(ctx)
--     local src = assert(ctx.inputs.read("src/a.lua"))
--     local w = ctx.outputs.create_blob("out")
--     w:write(src) ; w:close()
--     ctx.outputs.declare_generated_output({ name = "./a.lua" }, w)
--   end)
--
-- Under `hull test` the transform also runs against a stub host held in
-- memory (test.run_transform, §13.6): the same ctx, the same calls, answered
-- from the test's spec instead of a build host. The stub is a test double:
-- the host's own checks (limits, names, grants) are the real host's, which
-- `hull plugins run` exercises.

-- The ctx of one action. call(op, args) answers { ok } / { error } / {};
-- bytes(s) is what a write sends.
local function make_ctx(action, call, bytes_of)
    -- Writer / tree handle -> its host id. Keyed by the object the plugin
    -- holds, so a handle cannot be made up.
    local handle_of = setmetatable({}, { __mode = "k" })

    local function rpc(op, args)
        local r = call(op, args)
        if type(r) ~= "table" then return nil, "invalid_reply" end
        if r.error ~= nil then return nil, r.error end
        if r.ok == nil then return true end
        return r.ok
    end

    local function bytes(b)
        if type(b) ~= "string" then error("bytes must be a string", 3) end
        return bytes_of(b)
    end

    local function writer(kind, id)
        local w = {}
        if kind == "blob" then
            function w:write(b) return rpc("blob.write", { handle = id, bytes = bytes(b) }) end
            function w:close() return rpc("blob.close", { handle = id }) end
        else
            function w:write(path, b, mode)
                return rpc("tree.write", { handle = id, path = path, bytes = bytes(b), mode = mode })
            end
            function w:close() return rpc("tree.close", { handle = id }) end
        end
        handle_of[w] = id
        return w
    end

    local outputs = {}
    function outputs.create_blob(name)
        local id, err = rpc("outputs.create_blob", { name = name })
        if not id then return nil, err end
        return writer("blob", id)
    end
    function outputs.create_tree(name)
        local id, err = rpc("outputs.create_tree", { name = name })
        if not id then return nil, err end
        return writer("tree", id)
    end
    function outputs.declare_generated_output(desc, w)
        return rpc("outputs.declare_generated_output",
                   { descriptor = desc, handle = handle_of[w] })
    end
    function outputs.declare_artifact(desc, t)
        return rpc("outputs.declare_artifact", { descriptor = desc, handle = handle_of[t] })
    end
    return {
        meta = action.meta,
        config = action.config,
        inputs = {
            read = function(path) return rpc("inputs.read", { path = path }) end,
            stat = function(path, opts) return rpc("inputs.stat", { path = path, opts = opts }) end,
            list = function(path) return rpc("inputs.list", { path = path }) end,
        },
        outputs = outputs,
        diagnostics = {
            emit = function(d) return rpc("diagnostics.emit", { diagnostic = d }) end,
        },
        source = {
            units = function() return rpc("source.units") end,
            -- A unit of units(), or its id: { bytes?, refusals }.
            erased_source = function(unit)
                if type(unit) == "table" then unit = rawget(unit, "id") end
                return rpc("source.erased_source", { unit = unit })
            end,
        },
    }
end

-- The stub host: spec.inputs (path -> bytes), spec.units, spec.erased
-- (unit id -> bytes, or { refusals = {...} }). It records what the transform writes, declares and emits.
local function stub_host(spec)
    local inputs = type(spec.inputs) == "table" and spec.inputs or {}
    local handles, next_id = {}, 0
    local outputs, diagnostics = {}, {}

    local function is_dir(path)
        if path == "." then return true end
        for p in pairs(inputs) do
            if type(p) == "string" and p:sub(1, #path + 1) == path .. "/" then return true end
        end
        return false
    end

    local ops = {}
    ops["inputs.read"] = function(a)
        if type(a.path) ~= "string" then return nil, "invalid_args" end
        local b = inputs[a.path]
        if b == nil then return nil, is_dir(a.path) and "not_a_regular_file" or "not_found" end
        return b
    end
    ops["inputs.stat"] = function(a)
        if type(a.path) ~= "string" then return nil, "invalid_args" end
        local o = type(a.opts) == "table" and a.opts or {}
        local b = inputs[a.path]
        if b ~= nil then
            return { type = "file", size = o.size and #b or nil, exec = o.exec and false or nil }
        end
        return { type = is_dir(a.path) and "dir" or "absent" }
    end
    ops["inputs.list"] = function(a)
        if type(a.path) ~= "string" then return nil, "invalid_args" end
        if not is_dir(a.path) then return nil, inputs[a.path] and "not_a_directory" or "not_found" end
        local prefix = a.path == "." and "" or (a.path .. "/")
        local seen, out = {}, {}
        for p in pairs(inputs) do
            if type(p) == "string" and p:sub(1, #prefix) == prefix then
                local rest = p:sub(#prefix + 1)
                local name, more = rest:match("^([^/]+)(/?)")
                if name and not seen[name] then
                    seen[name] = true
                    out[#out + 1] = { name = name, type = more == "/" and "dir" or "file" }
                end
            end
        end
        table.sort(out, function(x, y) return x.name < y.name end)
        return out
    end
    local function create(kind)
        return function(a)
            if type(a.name) ~= "string" then return nil, "invalid_args" end
            next_id = next_id + 1
            handles[next_id] = { kind = kind, name = a.name, chunks = {}, entries = {}, open = true }
            return next_id
        end
    end
    ops["outputs.create_blob"] = create("blob")
    ops["outputs.create_tree"] = create("tree")
    local function open_handle(a, kind)
        local h = handles[a.handle]
        if not h or h.kind ~= kind then return nil, "invalid_handle" end
        if not h.open then return nil, "closed" end
        return h
    end
    ops["blob.write"] = function(a)
        local h, e = open_handle(a, "blob")
        if not h then return nil, e end
        h.chunks[#h.chunks + 1] = a.bytes
    end
    ops["tree.write"] = function(a)
        local h, e = open_handle(a, "tree")
        if not h then return nil, e end
        h.entries[#h.entries + 1] = { path = a.path, bytes = a.bytes, mode = a.mode or "regular" }
    end
    local function close(kind)
        return function(a)
            local h, e = open_handle(a, kind)
            if not h then return nil, e end
            h.open = false
        end
    end
    ops["blob.close"] = close("blob")
    ops["tree.close"] = close("tree")
    local function declare(declared)
        return function(a)
            local h = handles[a.handle]
            if not h then return nil, "invalid_handle" end
            if h.open then return nil, "open_writer" end
            local o = { declared = declared, descriptor = a.descriptor, name = h.name }
            if h.kind == "blob" then o.bytes = table.concat(h.chunks) else o.entries = h.entries end
            outputs[#outputs + 1] = o
        end
    end
    ops["outputs.declare_generated_output"] = declare("generated")
    ops["outputs.declare_artifact"] = declare("artifact")
    ops["diagnostics.emit"] = function(a)
        if type(a.diagnostic) ~= "table" then return nil, "invalid_diagnostic" end
        diagnostics[#diagnostics + 1] = a.diagnostic
    end
    ops["source.units"] = function() return type(spec.units) == "table" and spec.units or {} end
    ops["source.erased_source"] = function(a)
        if type(a.unit) ~= "string" then return nil, "invalid_args" end
        local b = type(spec.erased) == "table" and spec.erased[a.unit]
        if b == nil or b == false then return nil, "unsupported_op" end
        if type(b) == "table" and type(b.refusals) == "table" then return { refusals = b.refusals } end
        return { bytes = b, refusals = {} }
    end

    local function call(op, args)
        local fn = ops[op]
        if not fn then return { error = "unknown_op" } end
        local v, e = fn(args or {})
        if e then return { error = e } end
        if v == nil then return {} end
        return { ok = v }
    end
    return call, outputs, diagnostics
end

return function(app, chan)
    local transform

    function app.transform(fn)
        if type(fn) ~= "function" then error("app.transform requires a function", 2) end
        transform = fn
        app.main(function()
            while true do
                local action = chan.next()
                if action == nil then return 0 end
                local ok, err = pcall(fn, make_ctx(action, chan.call, chan.bytes))
                chan.done(not ok and err or nil)
            end
        end)
    end

    -- test.run_transform(spec): the registered transform against the stub
    -- host. Returns { outputs, diagnostics, error? }.
    return function(spec)
        if not transform then error("test.run_transform: the app registered no app.transform", 2) end
        if type(spec) ~= "table" then spec = {} end
        local call, outputs, diagnostics = stub_host(spec)
        local action = { config = spec.config or {}, meta = spec.meta or {} }
        local ok, err = pcall(transform, make_ctx(action, call, function(s) return s end))
        return { outputs = outputs, diagnostics = diagnostics, error = not ok and tostring(err) or nil }
    end
end
