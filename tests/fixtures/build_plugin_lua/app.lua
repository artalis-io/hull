-- A build plugin (tests/e2e_build_plugin.sh); app.js in build_plugin_js is
-- the same transform in JavaScript and must drive the host identically.
app.manifest({ modules = { "hull/build-plugin@1" } })

app.transform(function(ctx)
    local src = assert(ctx.inputs.read("src/a.txt"))
    local w = assert(ctx.outputs.create_blob("out"))
    assert(w:write(ctx.config.greeting .. ":"))
    assert(w:write(src))
    assert(w:close())
    assert(ctx.outputs.declare_generated_output({ name = "./a.txt" }, w))
    assert(ctx.diagnostics.emit({ severity = "info", code = "demo.unit",
                                  message = "unit " .. ctx.meta.unit }))
    local none, err = ctx.inputs.read("../escape")
    assert(none == nil and err == "denied", "a denied read is a token")
    if ctx.meta.unit == "b" then error("unit b fails", 0) end
end)
