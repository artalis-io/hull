-- The demonstration app in Lua; app_js/app.js is the same app.
app.manifest({})

---@demo greeting
local function hello() return "hello" end

---@demo parting
local function bye() return "bye" end

---@other not selected
local function other() return "other" end

app.main(function()
    print(hello(), bye(), other())
    return 0
end)
