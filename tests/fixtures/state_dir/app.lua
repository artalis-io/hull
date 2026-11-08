-- The per-user state directory (shared/state_dir.h, platform gap 3): a tool
-- keeps its state under the "@state/" root, wherever it is run from.
local fs = require("hull.fs")
app.manifest({
    name = "state-dir-e2e",
    state = { name = "hull-e2e-state" },
    modules = { "hull/fs@1" },
    fs = { read = { "@state/" }, write = { "@state/keys/", "@state/notes.txt" } },
})
app.main(function(ctx)
    local mode = ctx.args[1] or "roundtrip"
    if mode == "traverse" then
        local r, err = fs.read("@state/../outside.txt")
        print("traverse=" .. tostring(r == nil and err ~= nil))
        local r2, err2 = fs.read("@other/x")
        print("other_root=" .. tostring(r2 == nil and err2 ~= nil))
        local r3, err3 = fs.write("@state/.hull/x", "no")
        print("state_hull=" .. tostring(r3 == nil and err3 ~= nil))
        return 0
    end
    assert(fs.write("@state/keys/trust.key", "secret-bytes", { private = true }))
    assert(fs.write("@state/notes.txt", "hello state"))
    print("read_back=" .. tostring(fs.read("@state/keys/trust.key")))
    print("notes=" .. tostring(fs.read("@state/notes.txt")))
    local st = fs.stat("@state/keys/trust.key")
    print("stat_type=" .. tostring(st and st.type))
    local names = {}
    for _, e in ipairs(fs.list("@state")) do names[#names + 1] = e.name end
    table.sort(names)
    print("listed=" .. table.concat(names, ","))
    -- The app root is not the state root: an app-relative path is a different
    -- namespace, with no grant here.
    local r, err = fs.read("keys/trust.key")
    print("app_root_denied=" .. tostring(r == nil and err ~= nil))
    return 0
end)
