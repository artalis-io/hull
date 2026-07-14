-- Health check + ETag example
--
-- Demonstrates hull.web.middleware.health and hull.web.middleware.etag

local db     = require("hull.db").default()
local health = require("hull.web.middleware.health")
local etag   = require("hull.web.middleware.etag")

local json = require("hull.json")
app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/json@1",
        "hull/crypto@1",
        "hull/db@1",
        "hull/time@1",
        "hull/web/middleware/etag@1",
        "hull/web/middleware/health@1",
    },
})

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
