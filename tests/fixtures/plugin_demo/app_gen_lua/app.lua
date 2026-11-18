-- The demonstration app that loads a generated file (tests/e2e_generation.sh):
-- inventory.json is not in the tree; the inventory plugin, configured with
-- `module`, generates it. app_gen_js/app.js is the same app.
app.manifest({})

---@demo greeting
local function hello() return "hello" end

---@demo parting
local function bye() return "bye" end

local inv = require("./inventory.json")

app.main(function(ctx)
    ctx.stdout:write(inv.title .. ": " .. table.concat(inv.items, ", ") ..
                     " (" .. hello() .. ", " .. bye() .. ")\n")
    return 0
end)
