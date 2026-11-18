-- The inventory build plugin in Lua; inventory_js/app.js is the same plugin.
app.manifest({
    modules = { "hull/build-plugin@1", "hull/json@1" },
    build_plugin = {
        id = "inventory", version = 1, input = "source",
        languages = { "lua", "javascript" }, annotations = { "demo" },
        output = "demo-inventory",
        config = { title = { type = "string", required = true },
                   module = { type = "string" } },
    },
})

app.transform(function(ctx)
    local lines = {}
    for _, u in ipairs(assert(ctx.source.units())) do
        assert(ctx.inputs.read(u.path))       -- the source it describes is a dependency
        for _, a in ipairs(u.annotations) do
            lines[#lines + 1] = u.name .. ": " .. (a.value or "")
        end
    end
    table.sort(lines)
    local w = assert(ctx.outputs.create_blob("inventory"))
    assert(w:write(ctx.config.title .. "\n" .. table.concat(lines, "\n") .. "\n"))
    assert(w:close())
    assert(ctx.outputs.declare_artifact({ name = "inventory", kind = "demo-inventory" }, w))
    -- With `module` set, the inventory is also an app file the app loads: a
    -- generated output, which `hull dev` and an app run from source are handed.
    if ctx.config.module then
        local json = require("hull.json")
        local items = {}
        for i, l in ipairs(lines) do items[i] = json.encode(l) end
        local m = assert(ctx.outputs.create_blob("module"))
        assert(m:write('{"items":[' .. table.concat(items, ",") .. '],"title":' ..
                       json.encode(ctx.config.title) .. "}\n"))
        assert(m:close())
        assert(ctx.outputs.declare_generated_output({ name = ctx.config.module,
                                                      position_map = "none" }, m))
    end
    assert(ctx.diagnostics.emit({ severity = "info", code = "inventory.count",
                                  message = #lines .. " declarations" }))
end)
