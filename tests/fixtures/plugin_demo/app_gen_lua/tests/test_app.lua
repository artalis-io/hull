-- hull test runs the build pipeline first: inventory.json is generated.
test("the generated inventory is loaded", function()
    local r = test.run_main({})
    test.eq(r.code, 0)
    test.eq(r.stdout, "Inventory: bye: parting, hello: greeting (hello, bye)\n")
end)
