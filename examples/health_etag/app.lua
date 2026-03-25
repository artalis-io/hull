-- Health check + ETag example
--
-- Demonstrates hull.middleware.health and hull.middleware.etag

local health = require("hull.middleware.health")
local etag = require("hull.middleware.etag")

-- Register a custom health check
health.register("app_ready", function()
    return true
end)

-- Register health middleware on all GET paths
app.use("GET", "/*", health.middleware())

-- API routes with ETag support
app.get("/api/items", function(req, res)
    local items = db.query("SELECT * FROM items ORDER BY id")
    etag.json(req, res, { items = items })
end)

app.get("/api/greeting", function(req, res)
    local name = req.query.name or "World"
    etag.text(req, res, "Hello, " .. name .. "!")
end)

app.get("/api/page", function(req, res)
    etag.html(req, res, "<h1>Hello</h1>")
end)

-- POST route (ETag skipped for non-GET)
app.post("/api/items", function(req, res)
    local body = json.decode(req.body)
    db.exec("INSERT INTO items (name) VALUES (?)", { body.name })
    res:status(201):json({ ok = true })
end)
