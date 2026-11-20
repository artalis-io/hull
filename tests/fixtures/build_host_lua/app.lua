-- A build plugin driven by the real host (stdlib/cli/lua/hull/buildctx/tests/
-- test_plugin.lua); app.js in build_host_js is the same transform in
-- JavaScript and must commit a byte-identical result.
app.manifest({
    modules = { "hull/build-plugin@1" },
    build_plugin = {
        id = "demo", version = 1, input = "source", languages = { "lua", "javascript" },
        output = "generated", config = { greeting = { type = "string", required = true } },
    },
})

app.transform(function(ctx)
    local mode = ctx.meta.mode
    if mode == "fail" then error("boom", 0) end
    if mode == "spin" then while true do end end
    -- Audit 49: the channel's edges (stdlib/cli/lua/hull/buildctx/tests/
    -- test_plugin_audit49.lua).
    if mode == "bigerr" then error(string.rep("A", 4095) .. "\xc3\xa9" .. string.rep("B", 70000), 0) end
    if mode == "nilerr" then error(nil) end
    if mode == "deep" or mode == "wide" then
        local d
        if mode == "deep" then              -- 62 levels: one past what the host takes
            d = { "x" }
            for _ = 1, 61 do d = { d } end
        else                                -- a DAG of 100^3 shared empty tables
            d = {}
            for _ = 1, 3 do
                local up = {}
                for i = 1, 100 do up[i] = d end
                d = up
            end
        end
        ctx.diagnostics.emit({ severity = "info", code = "c", message = "m", data = d })
        return
    end
    local src = assert(ctx.inputs.read("src/a.txt"))
    local st = assert(ctx.inputs.stat("src/a.txt", { size = true }))
    local list = assert(ctx.inputs.list("src"))
    local empty = assert(ctx.inputs.list("empty"))
    local units = assert(ctx.source.units())
    local w = assert(ctx.outputs.create_blob("out"))
    assert(w:write(ctx.config.greeting .. ":"))
    assert(w:write(src))
    assert(w:write(":" .. st.size .. ":" .. #list .. ":" .. #empty .. ":" .. #units))
    assert(w:close())
    assert(ctx.outputs.declare_generated_output({ name = "./src/a.txt" }, w))
    local t = assert(ctx.outputs.create_tree("assets"))
    assert(t:write("x/y.txt", "y", "regular"))
    assert(t:close())
    assert(ctx.outputs.declare_artifact({ name = "assets", kind = "demo-tree" }, t))
    assert(ctx.diagnostics.emit({ severity = "warning", code = "demo.unit",
                                  message = "unit " .. units[1] }))
    local _, e1 = ctx.inputs.read("../x")
    assert(e1 == "invalid_path", "a path outside the grants is a token")
    local _, e2 = ctx.inputs.read("src/missing.txt")
    assert(e2 == "not_found", "a missing file is an observation")
    local _, e3 = w:write("late")
    assert(e3 == "closed", "a closed writer refuses")
    if mode == "undeclared" then assert(ctx.outputs.create_blob("loose")):close() end
end)
