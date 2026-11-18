-- The inventory transform against a stub host (test.run_transform): the
-- same ctx calls, answered from this spec instead of a build host.

local UNITS = {
    { id = "lua:app.lua:function:hello:1-2", language = "lua", path = "app.lua", kind = "function",
      name = "hello", annotations = { { name = "demo", value = "greeting" } } },
    { id = "lua:app.lua:function:bye:3-4", language = "lua", path = "app.lua", kind = "function",
      name = "bye", annotations = { { name = "demo", value = "parting" } } },
}

test("the inventory lists the units it is shown", function()
    local r = test.run_transform({
        config = { title = "Inventory" }, units = UNITS, inputs = { ["app.lua"] = "-- app\n" },
    })
    test.eq(r.error, nil)
    test.eq(#r.outputs, 1)
    test.eq(r.outputs[1].declared, "artifact")
    test.eq(r.outputs[1].descriptor.kind, "demo-inventory")
    test.eq(r.outputs[1].bytes, "Inventory\nbye: parting\nhello: greeting\n")
    test.eq(#r.diagnostics, 1)
    test.eq(r.diagnostics[1].code, "inventory.count")
end)

test("a unit whose source cannot be read fails the transform", function()
    local r = test.run_transform({ config = { title = "X" }, units = UNITS, inputs = {} })
    test.ok(r.error and r.error:find("not_found", 1, true), tostring(r.error))
    test.eq(#r.outputs, 0)
end)
