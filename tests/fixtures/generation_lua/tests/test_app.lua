-- Passes only when the generation's lib/m.lua shadows the file.
test("lib/m comes from the generation", function()
    local r = test.run_main({})
    test.eq(r.code, 0)
    test.eq(r.stdout, "entry=disk m=overlay page=disk-page\n")
end)
