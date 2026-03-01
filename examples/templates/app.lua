--
-- Template engine example — Lua
--
-- Demonstrates inheritance, includes, filters, loops, and CSP nonce.
--
-- Run:  hull dev examples/templates/app.lua
--       hull examples/templates/app.lua
--

local template = require("hull.template")

-- Sample data
local users = {
    { name = "Alice",   email = "alice@example.com" },
    { name = "Bob",     email = "bob@example.com" },
    { name = "Charlie", email = nil },
}

local features = {
    "  Template inheritance  ",
    "  Include partials  ",
    "  Built-in filters  ",
    "  HTML auto-escaping  ",
    "  Compiled & cached  ",
}

app.get("/", function(req, res)
    local html = template.render("pages/home.html", {
        site_name    = "Hull Demo",
        year         = os.date("%Y"),
        users        = users,
        features     = features,
        html_snippet = '<em>bold & "quoted"</em>',
    })
    res:html(html)
end)

app.get("/about", function(req, res)
    local html = template.render("pages/about.html", {
        site_name = "Hull Demo",
        year      = os.date("%Y"),
        version   = "0.1.0",
    })
    res:html(html)
end)

app.get("/users", function(req, res)
    res:json(users)
end)
