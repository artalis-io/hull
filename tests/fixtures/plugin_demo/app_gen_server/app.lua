-- The demonstration app as a server, for `hull dev` (tests/e2e_generation.sh):
-- GET / answers the inventory the plugin generates as inventory.json.
app.manifest({ modules = { "hull/http-server@1" } })

---@demo greeting
local function hello() return "hello" end

---@demo parting
local function bye() return "bye" end

local inv = require("./inventory.json")

app.get("/", function(_req, res)
    res:text(inv.title .. ": " .. table.concat(inv.items, ", ") ..
             " (" .. hello() .. ", " .. bye() .. ")\n")
end)
