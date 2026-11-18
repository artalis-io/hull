-- The inventory build plugin in Lua; inventory_js/app.js is the same plugin.
app.manifest({
    modules = { "hull/build-plugin@1" },
    build_plugin = {
        id = "inventory", version = 1, input = "source",
        languages = { "lua", "javascript" }, annotations = { "demo" },
        output = "demo-inventory",
        config = { title = { type = "string", required = true } },
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
    assert(ctx.diagnostics.emit({ severity = "info", code = "inventory.count",
                                  message = #lines .. " declarations" }))
end)
