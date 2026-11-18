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

return function(app, chan)
    -- Writer / tree handle -> its host id. Keyed by the object the plugin
    -- holds, so a handle cannot be made up.
    local handle_of = setmetatable({}, { __mode = "k" })

    local function rpc(op, args)
        local r = chan.call(op, args)
        if type(r) ~= "table" then return nil, "invalid_reply" end
        if r.error ~= nil then return nil, r.error end
        if r.ok == nil then return true end
        return r.ok
    end

    local function bytes(b)
        if type(b) ~= "string" then error("bytes must be a string", 3) end
        return chan.bytes(b)
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

    local function make_ctx(action)
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
                erased_source = function(unit) return rpc("source.erased_source", { unit = unit }) end,
            },
        }
    end

    function app.transform(fn)
        if type(fn) ~= "function" then error("app.transform requires a function", 2) end
        app.main(function()
            while true do
                local action = chan.next()
                if action == nil then return 0 end
                local ok, err = pcall(fn, make_ctx(action))
                chan.done(not ok and err or nil)
            end
        end)
    end
end
